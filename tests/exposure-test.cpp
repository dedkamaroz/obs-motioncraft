/* Standalone check of the exposure drift. Transcribed from stepExposureChannel()
 * and advanceExposure() in motioncraft-controller.cpp so it can run without OBS.
 *
 * The properties under test are the ones the effect lives or dies by, and the
 * ones the user asked for:
 *   - no camera motion produces no drift (it must cost nothing when still);
 *   - the drift is DIRECTION-dependent: moving one way and the opposite way give
 *     oppositely-signed drift, not the same magnitude (the whole point of the
 *     rework - direction used to be random noise);
 *   - a burst of motion does not move a control on the very next frame - it
 *     lags, which is the "slight delay";
 *   - once the camera stops, each control returns to neutral, overshooting zero
 *     on the way (the auto-exposure "hunting", not a one-way fade);
 *   - the two controls (level, spread) are independent;
 *   - the Amount dial scales both, and zero means off.
 *
 *   cl /EHsc /O2 tests\exposure-test.cpp
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

static int g_failures = 0;

static void expect(bool cond, const char *what)
{
	std::printf("%-66s %s\n", what, cond ? "ok" : "FAILED");
	if (!cond)
		++g_failures;
}

static inline double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static const double PI = 3.14159265358979323846;

/* ---- transcribed from motioncraft-controller.cpp ---- */

static const double kExposureReactTau = 0.15;
static const double kExposureSettleSec = 0.70;
static const double kExposureDamping = 0.55;
static const double kExposureSpeedRef = 1.2;
static const double kExposureMaxSubstepSec = 1.0 / 120.0;
static const double kExposureMaxBrightness = 0.18;
static const double kExposureMaxContrast = 0.30;
static const double kExposureAmountMax = 100.0;

struct ExpoChannel {
	double activity = 0.0;
	double value = 0.0;
	double vel = 0.0;
};

static double stepExposureChannel(ExpoChannel &ch, double signedDrive, double seconds)
{
	const double target = std::tanh(signedDrive / kExposureSpeedRef);
	const double omega = 2.0 * PI / kExposureSettleSec;

	int steps = (int)std::ceil(seconds / kExposureMaxSubstepSec);
	if (steps < 1)
		steps = 1;
	const double h = seconds / steps;
	const double reactAlpha = 1.0 - std::exp(-h / kExposureReactTau);
	for (int i = 0; i < steps; ++i) {
		ch.activity += (target - ch.activity) * reactAlpha;
		const double accel = omega * omega * (ch.activity - ch.value) - 2.0 * kExposureDamping * omega * ch.vel;
		ch.vel += accel * h;
		ch.value += ch.vel * h;
	}
	ch.value = clampd(ch.value, -1.5, 1.5);
	return ch.value;
}

static double brightnessOf(double level, double amount)
{
	const double amount01 = clampd(amount, 0.0, kExposureAmountMax) / kExposureAmountMax;
	return clampd(level * kExposureMaxBrightness * amount01, -1.0, 1.0);
}
static double contrastOf(double spread, double amount)
{
	const double amount01 = clampd(amount, 0.0, kExposureAmountMax) / kExposureAmountMax;
	return clampd(spread * kExposureMaxContrast * amount01, -1.0, 1.0);
}

/* Run a channel for a push of `drive` held for pushFrames, then still for
 * stillFrames. Reports first-frame magnitude, peak, whether it overshot the far
 * side of zero on the way back, and where it settled. */
struct Run {
	double first, peak, settled;
	bool overshot;
};
static Run runChannel(double drive, double pushSec, double stillSec, double dt)
{
	ExpoChannel ch;
	Run out{};
	const int pushFrames = (int)std::lround(pushSec / dt);
	const int stillFrames = (int)std::lround(stillSec / dt);
	out.first = std::fabs(stepExposureChannel(ch, drive, dt));
	out.peak = out.first;
	for (int i = 1; i < pushFrames; ++i)
		out.peak = std::max(out.peak, std::fabs(stepExposureChannel(ch, drive, dt)));

	const double pushSign = ch.value >= 0 ? 1.0 : -1.0;
	out.overshot = false;
	for (int i = 0; i < stillFrames; ++i) {
		out.settled = stepExposureChannel(ch, 0.0, dt);
		if (out.settled * pushSign < -1e-3)
			out.overshot = true;
	}
	return out;
}

int main()
{
	/* Still camera: no drift. */
	{
		ExpoChannel ch;
		double worst = 0.0;
		for (int i = 0; i < 600; ++i)
			worst = std::max(worst, std::fabs(stepExposureChannel(ch, 0.0, 1.0 / 60.0)));
		expect(worst < 1e-6, "still camera leaves the drift at neutral");
	}

	/* Direction-dependence: a positive drive and its exact negative must give
	 * oppositely-signed, equal-magnitude drift. This is the property that was
	 * broken when the direction came from random noise. */
	{
		ExpoChannel cp, cn;
		double vp = 0.0, vn = 0.0;
		for (int i = 0; i < 30; ++i) {
			vp = stepExposureChannel(cp, +3.0, 1.0 / 60.0);
			vn = stepExposureChannel(cn, -3.0, 1.0 / 60.0);
		}
		expect(vp > 0.1 && vn < -0.1, "opposite directions drift opposite ways");
		expect(std::fabs(vp + vn) < 1e-9, "opposite directions are equal and opposite");
	}

	/* Lag, hunting, settle - and the same hunting at 20 fps as at 60/240, which
	 * the single-step integration lost (the reason the spring now substeps). */
	{
		Run r = runChannel(+3.0, 0.5, 4.0, 1.0 / 60.0);
		expect(r.first < 0.1 * r.peak, "a control barely moves on the first frame (lag)");
		expect(r.peak > 0.1, "a control moves while the camera moves");
		expect(r.overshot, "a control overshoots zero settling back (hunting)");
		expect(std::fabs(r.settled) < 1e-3, "a control returns to neutral once still");

		Run r20 = runChannel(+3.0, 0.5, 4.0, 1.0 / 20.0);
		Run r240 = runChannel(+3.0, 0.5, 4.0, 1.0 / 240.0);
		expect(r20.overshot, "overshoot survives at 20 fps (frame-rate independent)");
		expect(r240.overshot, "overshoot present at 240 fps");
		expect(std::fabs(r20.peak - r240.peak) < 0.15, "peak is close across 20 and 240 fps");
	}

	/* Two controls are independent: projecting a pure-X motion onto a level gain
	 * that is all-Y and a spread gain that is all-X drives spread, not level. */
	{
		const double vx = 2.0, vy = 0.0;
		const double levelGx = 0.0, levelGy = 1.0;  /* level reads vertical only */
		const double spreadGx = 1.0, spreadGy = 0.0; /* spread reads horizontal only */
		ExpoChannel lvl, spr;
		double lv = 0.0, sv = 0.0;
		for (int i = 0; i < 60; ++i) {
			lv = stepExposureChannel(lvl, vx * levelGx + vy * levelGy, 1.0 / 60.0);
			sv = stepExposureChannel(spr, vx * spreadGx + vy * spreadGy, 1.0 / 60.0);
		}
		expect(std::fabs(lv) < 1e-9, "motion off a control's axis leaves it untouched");
		expect(std::fabs(sv) > 0.1, "motion on a control's axis drives it");
	}

	/* Amount scales both outputs, zero is off, full stays within the ceilings. */
	{
		expect(std::fabs(brightnessOf(1.0, 0.0)) < 1e-9, "amount 0 yields no brightness change");
		expect(std::fabs(contrastOf(1.0, 0.0)) < 1e-9, "amount 0 yields no contrast change");
		expect(brightnessOf(1.0, 100.0) > brightnessOf(1.0, 50.0), "amount scales the brightness swing");
		expect(contrastOf(1.0, 100.0) > contrastOf(1.0, 50.0), "amount scales the contrast swing");
		expect(brightnessOf(1.0, 100.0) <= kExposureMaxBrightness + 1e-9, "brightness stays within its ceiling");
		expect(contrastOf(1.0, 100.0) <= kExposureMaxContrast + 1e-9, "contrast stays within its ceiling");
	}

	std::printf("\n%s\n", g_failures ? "FAILURES" : "all ok");
	return g_failures ? 1 : 0;
}
