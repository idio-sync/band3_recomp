#pragma once
#include <cstdint>
#include <string>

// band3 telling Home Assistant what the game is doing, as the RB3E Dashboard
// does: entities over MQTT discovery (ha_mqtt_host) and the dashboard's
// webhook payloads (ha_webhook_url). Home Assistant only watches; nothing it
// sends reaches the game. ha_entities.h has what is said, this puts it on the
// wire from threads of its own, reading what the hooks keep in
// band3::test::GameState.

namespace band3::ha {

// from band3_app.h beside discord::Start(); does nothing when neither a host
// nor a webhook URL is set
void Start();
// publishes "offline" and disconnects, and ends the webhook's threads (without
// waiting long on a POST in flight); safe to call more than once
void Stop();
// a host or a webhook URL was set at startup: the hooks keep GameState for it
bool Configured();
// the MQTT connection's state, as mqtt::StatusText says it; "off" without a host
std::string StateName();
// StageKit::SetState(left, right), from the game thread: applied to the
// lights HA is shown when ha_stagekit is on. Lock-free, never waits.
void NoteStageKit(uint8_t left, uint8_t right);

}  // namespace band3::ha
