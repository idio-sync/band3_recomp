"""publish_nightly: puts package.py's zips on the repository's rolling nightly release.

  python tools/publish_nightly.py out/package/slopband3-<commit>-win-amd64-release.zip
  python tools/publish_nightly.py --dispatch-linux <commit>

The release is tagged nightly and marked a prerelease; it's made on first use.
A zip replaces the release's older zips from the same build folder (the name
after the commit), so it holds one zip per platform. --dispatch-linux starts
.github/workflows/nightly-linux.yml for that commit, which builds the Linux zip
on GitHub and publishes it with this script: a full Linux build there takes
about two hours, one whose generated code sccache already has much less.

The token is GITHUB_TOKEN or GH_TOKEN, else git's stored github.com
credential (git credential fill, which never prompts here).

Standard library only.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAG = "nightly"
WORKFLOW = "nightly-linux.yml"
API = "https://api.github.com"
UPLOADS = "https://uploads.github.com"
ZIP_NAME = re.compile(r"slopband3-[0-9a-f]+(?:-dirty)?-(.+)\.zip")
NOTES = ("Rolling builds of main, one zip per platform: each zip's name has the commit "
         "it was built from. No game files: see README.txt in the zip.")


def git(*args, stdin=None):
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0", GCM_INTERACTIVE="never")
    return subprocess.run(["git", *args], cwd=REPO, input=stdin, capture_output=True,
                          text=True, env=env, check=True).stdout


def slug():
    """owner/name of origin, from its https or ssh address."""
    url = git("remote", "get-url", "origin").strip()
    m = re.match(r"(?:https://github\.com/|git@github\.com:)([^/]+/[^/]+?)(?:\.git)?$", url)
    if not m:
        sys.exit(f"origin isn't a GitHub repository: {url}")
    return m.group(1)


def token():
    for name in ("GITHUB_TOKEN", "GH_TOKEN"):
        if os.environ.get(name):
            return os.environ[name]
    try:
        out = git("credential", "fill", stdin="protocol=https\nhost=github.com\n\n")
    except subprocess.CalledProcessError:
        out = ""
    for line in out.splitlines():
        if line.startswith("password="):
            return line[len("password="):]
    sys.exit("no GitHub token: set GITHUB_TOKEN, or sign git in to github.com")


class GitHub:
    def __init__(self):
        self.repo = slug()
        self.token = token()

    def call(self, method, url, body=None, content_type="application/json"):
        if isinstance(body, dict):
            body = json.dumps(body).encode()
        req = urllib.request.Request(url, data=body, method=method, headers={
            "Authorization": f"Bearer {self.token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            **({"Content-Type": content_type} if body is not None else {}),
        })
        try:
            with urllib.request.urlopen(req) as r:
                data = r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404 and method == "GET":
                return None
            sys.exit(f"{method} {url}: {e.code} {e.read().decode(errors='replace')}")
        return json.loads(data) if data else {}

    def api(self, method, path, body=None):
        return self.call(method, f"{API}/repos/{self.repo}{path}", body)

    def release(self, commit):
        """The nightly release, made at commit if there's none."""
        found = self.api("GET", f"/releases/tags/{TAG}")
        if found:
            return found
        return self.api("POST", "/releases", {
            "tag_name": TAG, "target_commitish": commit, "name": "Nightly",
            "body": NOTES, "prerelease": True})

    def assets(self, release):
        return self.api("GET", f"/releases/{release['id']}/assets?per_page=100") or []


def publish(gh, path):
    name = os.path.basename(path)
    m = ZIP_NAME.fullmatch(name)
    if not m:
        sys.exit(f"{name} isn't one of package.py's zips (slopband3-<commit>-<build folder>.zip)")
    build = m.group(1)
    release = gh.release(git("rev-parse", "HEAD").strip())
    old = [a for a in gh.assets(release)
           if (n := ZIP_NAME.fullmatch(a["name"])) and n.group(1) == build]
    # the same name again (a rebuilt commit) has to go before the upload
    for a in old:
        if a["name"] == name:
            gh.api("DELETE", f"/releases/assets/{a['id']}")
    with open(path, "rb") as f:
        data = f.read()
    url = (f"{UPLOADS}/repos/{gh.repo}/releases/{release['id']}/assets?"
           + urllib.parse.urlencode({"name": name}))
    asset = gh.call("POST", url, data, "application/zip")
    for a in old:
        if a["name"] != name:
            gh.api("DELETE", f"/releases/assets/{a['id']}")
            print(f"removed {a['name']}")
    print(asset["browser_download_url"])


def dispatch_linux(gh, commit):
    full = git("rev-parse", "--verify", f"{commit}^{{commit}}").strip()
    gh.api("POST", f"/actions/workflows/{WORKFLOW}/dispatches",
           {"ref": "main", "inputs": {"commit": full}})
    print(f"started {WORKFLOW} for {full[:7]}: https://github.com/{gh.repo}/actions/workflows/{WORKFLOW}")


def main(argv):
    p = argparse.ArgumentParser(description="Put package.py's zips on the nightly release.")
    p.add_argument("zips", nargs="*", help="zips from tools/package.py")
    p.add_argument("--dispatch-linux", metavar="COMMIT",
                   help="start the Linux nightly build on GitHub for COMMIT (pushed already)")
    args = p.parse_args(argv)
    if not args.zips and not args.dispatch_linux:
        p.error("nothing to do: give zips, --dispatch-linux, or both")
    gh = GitHub()
    for z in args.zips:
        publish(gh, z)
    if args.dispatch_linux:
        dispatch_linux(gh, args.dispatch_linux)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
