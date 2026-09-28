#include <cmath>
#include <rex/hook.h>
#include "src/config.h"

// Like REX_HOOK, but runs the recompiled original when [debug] native_math is off.
#define BAND3_MATH_HOOK(subroutine, function)                         \
  extern "C" void __imp__##subroutine(PPCContext& ctx, uint8_t* base); \
  extern "C" REX_FUNC(subroutine) {                                     \
    if (!band3::GetConfig().native_math) {                              \
      __imp__##subroutine(ctx, base);                                   \
      return;                                                           \
    }                                                                   \
    rex::ppc::HostToGuestFunction<function>(ctx, base);                 \
  }

// native replacements for recompiled PPC math functions

// Matrix/vector math stuff
// TODO: add proper matrix/vector3 structures to make this look cleaner

// Normalize(Vector3* src, Vector3* dst)
static void Normalize_Vector3(mapped_f32 src, mapped_f32 dst) {
	f32 x = src[0];
	f32 y = src[1];
	f32 z = src[2];

	if (x == 0.0f && y == 0.0f && z == 0.0f) {
		dst[0] = 0.0f;
		dst[1] = 0.0f;
		dst[2] = 0.0f;
		return;
	}

	f32 inv = 1.0f / std::sqrt(x * x + y * y + z * z);
	dst[0] = x * inv;
	dst[1] = y * inv;
	dst[2] = z * inv;
}

BAND3_MATH_HOOK(_Normalize_Vector3, Normalize_Vector3)

static void Normalize_Matrix3(mapped_f32 src, mapped_f32 dst) {
	Normalize_Vector3(src + 4, dst + 4);

	f32 r1x = dst[4];
	f32 r1y = dst[5];
	f32 r1z = dst[6];
	f32 r2x = src[8];
	f32 r2y = src[9];
	f32 r2z = src[10];

	dst[0] = r1y * r2z - r1z * r2y;
	dst[1] = r1z * r2x - r1x * r2z;
	dst[2] = r1x * r2y - r1y * r2x;
	Normalize_Vector3(dst, dst);

	f32 r0x = dst[0];
	f32 r0y = dst[1];
	f32 r0z = dst[2];

	dst[8] = r0y * r1z - r0z * r1y;
	dst[9] = r0z * r1x - r0x * r1z;
	dst[10] = r0x * r1y - r0y * r1x;
	Normalize_Vector3(dst + 8, dst + 8);
}

BAND3_MATH_HOOK(_Normalize_Matrix3, Normalize_Matrix3)

// Multiply(Matrix3* A, Matrix3* B, Matrix3* C)
static void Multiply_Matrix3(mapped_f32 src_a, mapped_f32 src_b, mapped_f32 dst) {
	be_f32 a[3][3], b[3][3];
	for (int i = 0; i < 3; i++) {
		a[i][0] = src_a[i*4];	a[i][1] = src_a[i*4 + 1];	a[i][2] = src_a[i*4 + 2];
		b[i][0] = src_b[i*4];	b[i][1] = src_b[i*4 + 1];	b[i][2] = src_b[i*4 + 2];
	}
	for (int i = 0; i < 3; i++) {
		f32 c1 = a[i][0]*b[0][0] + a[i][1]*b[1][0] + a[i][2]*b[2][0];
		f32 c2 = a[i][0]*b[0][1] + a[i][1]*b[1][1] + a[i][2]*b[2][1];
		f32 c3 = a[i][0]*b[0][2] + a[i][1]*b[1][2] + a[i][2]*b[2][2];

		dst[i*4] = c1;
		dst[i*4 + 1] = c2;
		dst[i*4 + 2] = c3;
	}
}

BAND3_MATH_HOOK(_Multiply_Matrix3, Multiply_Matrix3)


static void Interp_Vector3(mapped_f32 a, mapped_f32 b, f64 t, mapped_f32 dst) {
	if (t == 0.0f) {
		dst[0] = a[0];
		dst[1] = a[1];
		dst[2] = a[2];
		dst[3] = a[3];
		return;
	}

	if (t == 1.0f) {
		dst[0] = b[0];
		dst[1] = b[1];
		dst[2] = b[2];
		dst[3] = b[3];
		return;
	}

	f64 ax = a[0], ay = a[1], az = a[2];
	f64 bx = b[0], by = b[1], bz = b[2];
	dst[0] = (bx - ax) * t + ax;
	dst[1] = (by - ay) * t + ay;
	dst[2] = (bz - az) * t + az;
}

// Using REX_HOOK_RAW because REX_HOOK maps the registers wrong in this case.
// I think it tries to use f3 instead of f1.
extern "C" void __imp___Interp_Vector3(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(_Interp_Vector3) {
	if (!band3::GetConfig().native_math) {
		__imp___Interp_Vector3(ctx, base);
		return;
	}
	mapped_f32 a = mapped_f32(rex::memory::GuestPtr<be_f32*>(base, ctx.r3.u32), ctx.r3.u32);
	mapped_f32 b = mapped_f32(rex::memory::GuestPtr<be_f32*>(base, ctx.r4.u32), ctx.r4.u32);
	f64 t = f64(ctx.f1.f64);
	mapped_f32 dst = mapped_f32(rex::memory::GuestPtr<be_f32*>(base, ctx.r6.u32), ctx.r6.u32);

	Interp_Vector3(a, b, t, dst);
}

BAND3_MATH_HOOK(_acos, static_cast<f64(*)(f64)>(std::acos));
BAND3_MATH_HOOK(_asin, static_cast<f64(*)(f64)>(std::asin));
BAND3_MATH_HOOK(_atan, static_cast<f64(*)(f64)>(std::atan));
BAND3_MATH_HOOK(_atan2, static_cast<f64(*)(f64,f64)>(std::atan2));
BAND3_MATH_HOOK(_cos, static_cast<f64(*)(f64)>(std::cos));
BAND3_MATH_HOOK(_floor, static_cast<f64(*)(f64)>(std::floor));
BAND3_MATH_HOOK(_fmod, static_cast<f64(*)(f64,f64)>(std::fmod));
BAND3_MATH_HOOK(_pow, static_cast<f64(*)(f64,f64)>(std::pow));
BAND3_MATH_HOOK(_sin, static_cast<f64(*)(f64)>(std::sin));
BAND3_MATH_HOOK(_tan, static_cast<f64(*)(f64)>(std::tan));

