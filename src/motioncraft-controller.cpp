#include "motioncraft-controller.hpp"

#include <obs-frontend-api.h>
#include <obs.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QGuiApplication>
#include <QScreen>
#include <QCursor>
#include <QRect>
#include <QKeySequence>
#include <QKeyCombination>
#include <QRegularExpression>
#include <QDateTime>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPen>

#include <cmath>
#include <algorithm>

#include <QFileInfo>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>
#endif

#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#endif

#include "motioncraft-dialog.hpp"

#ifdef __linux__
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysymdef.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>
#include <X11/Xatom.h>

#undef Bool
#undef None
#undef Status
#undef CursorShape
#undef KeyPress
#undef KeyRelease
#undef FocusIn
#undef FocusOut
#undef Above
#undef Below
#undef Unsorted

static constexpr long X11_None = 0L;
#endif

static int qtKeyToVk(int qtKey);

static constexpr const char *kMotionCraftMarkerSourceName = "MotionCraft Cursor Marker";
static constexpr const char *kMotionCraftMarkerSourceId = "motioncraft_marker_source";
static void cleanup_legacy_marker_items_all_scenes(obs_source_t *currentMarkerSource = nullptr);
static bool source_name_starts_with(const char *name, const char *prefix);
static void collect_live_scene_items(obs_scene_t *scene, std::vector<obs_sceneitem_t *> &items);

static inline double clampd(double v, double lo, double hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static inline double smoothstep(double t)
{
	return t * t * (3.0 - 2.0 * t);
}

/* Cubic Hermite from (z0, v0) to (z1, zero velocity) over duration D seconds,
 * evaluated at u in [0,1]. Velocity is in zoom units per second.
 *
 * With v0 == 0 the basis collapses to
 *     z = z0 + (z1 - z0) * (3u^2 - 2u^3) = z0 + (z1 - z0) * smoothstep(u)
 * so an uninterrupted transition is the same ease-in/ease-out ramp the plugin
 * has always used. The v0 term only comes into play when a transition is
 * retargeted mid-flight: the new segment then starts at the speed the old one
 * had, instead of snapping to zero and re-accelerating, and still arrives at
 * rest. Departure velocity is continuous, so there is no visible jolt at the
 * seam; the trade is a bounded overshoot of at most ~0.148 * D * v0 when the
 * new target lies behind the direction of travel, which reads as momentum. */
static inline void hermite_ease(double u, double z0, double z1, double v0, double D, double &zOut, double &velOut)
{
	const double u2 = u * u;
	const double u3 = u2 * u;
	const double h00 = 2.0 * u3 - 3.0 * u2 + 1.0;
	const double h10 = u3 - 2.0 * u2 + u;
	const double h01 = -2.0 * u3 + 3.0 * u2;

	zOut = z0 * h00 + (D * v0) * h10 + z1 * h01;
	velOut = (D > 0.0) ? (((z1 - z0) * (6.0 * u - 6.0 * u2) + (D * v0) * (3.0 * u2 - 4.0 * u + 1.0)) / D) : 0.0;
}

static inline bool nearly_equal(float a, float b, float eps = 0.5f)
{
	return std::fabs(a - b) <= eps;
}

static inline bool nearly_equal_vec2(const vec2 &a, const vec2 &b, float eps = 0.01f)
{
	return nearly_equal(a.x, b.x, eps) && nearly_equal(a.y, b.y, eps);
}

/* Scene-item rotation, in the same sense libobs applies it (about +Z, degrees):
 *   x' = x*cos - y*sin
 *   y' = x*sin + y*cos
 */
static inline void rotation_sin_cos(float degrees, float &sinOut, float &cosOut)
{
	if (degrees == 0.0f) {
		sinOut = 0.0f;
		cosOut = 1.0f;
		return;
	}
	const double rad = (double)degrees * 3.14159265358979323846 / 180.0;
	sinOut = (float)std::sin(rad);
	cosOut = (float)std::cos(rad);
}

/* 32-bit avalanche mix (Murmur3 finaliser). The wiggle's noise needs
 * neighbouring integers to hash to unrelated values; a bare multiplicative
 * hash does not give that, because the low bits of k * K advance by a fixed
 * step as k increments, so successive samples land on an arithmetic ramp and
 * the "noise" comes out as a regular zigzag rather than a drift. */
static inline uint32_t wiggle_hash(uint32_t k)
{
	k ^= k >> 16;
	k *= 0x85ebca6bu;
	k ^= k >> 13;
	k *= 0xc2b2ae35u;
	k ^= k >> 16;
	return k;
}

/* Value noise in [-1, 1]: one random value per integer step of t, joined with
 * smoothstep. Continuous and zero-derivative at the knots, which is what makes
 * the motion read as a hand holding a camera rather than as a vibration. */
static inline double wiggle_noise(double t, uint32_t seed)
{
	const double base = std::floor(t);
	const double frac = t - base;
	const uint32_t i = (uint32_t)(int32_t)base;

	auto sample = [seed](uint32_t k) {
		return ((double)wiggle_hash(seed + k) / 4294967295.0) * 2.0 - 1.0;
	};

	const double v0 = sample(i);
	const double v1 = sample(i + 1u);
	return v0 + (v1 - v0) * smoothstep(frac);
}

/* Write-suppression threshold for scale. Position is in pixels, where the 0.01
 * default is a sane "nothing changed" epsilon, but scale is a ratio: 0.01 there
 * is 1% of the source, i.e. tens of pixels of rendered size. Sharing the pixel
 * epsilon made a slow zoom accumulate scale silently for several frames and
 * then apply it in one step, while position kept updating every frame. Small
 * enough here that even an 8K source stays under a hundredth of a pixel. */
static constexpr float kScaleEpsilon = 1.0e-5f;

static inline void logi(bool enabled, const char *fmt, ...)
{
	if (!enabled)
		return;
	va_list args;
	va_start(args, fmt);
	blogva(LOG_INFO, fmt, args);
	va_end(args);
}

static bool source_name_starts_with(const char *name, const char *prefix)
{
	if (!name || !prefix || !*prefix)
		return false;
	return QString::fromUtf8(name).startsWith(QString::fromUtf8(prefix));
}

static void collect_live_scene_items(obs_scene_t *scene, std::vector<obs_sceneitem_t *> &items)
{
	if (!scene)
		return;

	struct Ctx {
		std::vector<obs_sceneitem_t *> *items = nullptr;

		static bool enum_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
		{
			auto *ctx = static_cast<Ctx *>(param);
			if (!ctx || !ctx->items || !item)
				return true;

			ctx->items->push_back(item);

			obs_source_t *src = obs_sceneitem_get_source(item);
			obs_scene_t *subScene = src ? obs_scene_from_source(src) : nullptr;
			if (subScene)
				collect_live_scene_items(subScene, *ctx->items);

			return true;
		}
	};

	Ctx ctx{&items};
	obs_scene_enum_items(scene, Ctx::enum_cb, &ctx);
}

static bool scene_item_pointer_is_live(obs_scene_t *scene, obs_sceneitem_t *item)
{
	if (!scene || !item)
		return false;

	std::vector<obs_sceneitem_t *> liveItems;
	collect_live_scene_items(scene, liveItems);
	return std::find(liveItems.begin(), liveItems.end(), item) != liveItems.end();
}

MotionCraftController &MotionCraftController::instance()
{
	static MotionCraftController inst;
	return inst;
}

MotionCraftController::MotionCraftController()
{
	tickTimer.setInterval(16);
	tickTimer.setTimerType(Qt::PreciseTimer);
	connect(&tickTimer, &QTimer::timeout, this, &MotionCraftController::onTick);
}

MotionCraftController::~MotionCraftController()
{
	markerSource = nullptr;
}

QString MotionCraftController::configPath() const
{
	char *path = obs_module_config_path("motioncraft.json");
	if (!path)
		return {};
	QString p = QString::fromUtf8(path);
	bfree(path);
	return p;
}

/* Where Zoominator kept its settings, derived from our own path so it tracks
 * whatever profile directory OBS hands us. Read once, when no MotionCraft
 * config exists yet, so an existing Zoominator setup survives the rename. */
QString MotionCraftController::legacyConfigPath() const
{
	const QString p = configPath();
	if (p.isEmpty())
		return {};
	const QString legacy = QStringLiteral("zoominator/zoominator.json");
	const int cut = p.lastIndexOf(QStringLiteral("motioncraft/motioncraft.json"));
	if (cut < 0)
		return {};
	return p.left(cut) + legacy;
}

QString MotionCraftController::markerImagePath() const
{
	char *path = obs_module_config_path("motioncraft-cursor-marker.png");
	if (!path)
		return {};
	QString p = QString::fromUtf8(path);
	bfree(path);
	return p;
}

QString MotionCraftController::sceneItemKey(obs_sceneitem_t *item) const
{
	if (!item)
		return {};

	obs_source_t *src = obs_sceneitem_get_source(item);
	obs_scene_t *scene = obs_sceneitem_get_scene(item);
	obs_source_t *sceneSource = scene ? obs_scene_get_source(scene) : nullptr;

	const char *sceneName = sceneSource ? obs_source_get_name(sceneSource) : nullptr;
	const char *sourceName = src ? obs_source_get_name(src) : nullptr;
	const int64_t itemId = obs_sceneitem_get_id(item);

	if (!sceneName || !*sceneName || !sourceName || !*sourceName)
		return {};

	return QStringLiteral("%1::%2::%3")
		.arg(QString::fromUtf8(sceneName))
		.arg(QString::fromUtf8(sourceName))
		.arg(itemId);
}

MotionCraftController::OrigState MotionCraftController::readSceneItemTransform(obs_sceneitem_t *item) const
{
	OrigState state{};
	if (!item)
		return state;

	obs_sceneitem_get_pos(item, &state.pos);
	obs_sceneitem_get_scale(item, &state.scale);
	state.rot = obs_sceneitem_get_rot(item);
	state.align = obs_sceneitem_get_alignment(item);
	state.boundsType = obs_sceneitem_get_bounds_type(item);
	state.boundsAlign = obs_sceneitem_get_bounds_alignment(item);
	obs_sceneitem_get_bounds(item, &state.bounds);
	obs_sceneitem_get_crop(item, &state.crop);
	state.valid = true;
	return state;
}

void MotionCraftController::applySceneItemTransform(obs_sceneitem_t *item, const OrigState &state)
{
	if (!item || !state.valid)
		return;

	obs_sceneitem_set_pos(item, &state.pos);
	obs_sceneitem_set_scale(item, &state.scale);
	obs_sceneitem_set_rot(item, state.rot);
	obs_sceneitem_set_alignment(item, state.align);
	obs_sceneitem_set_bounds_type(item, state.boundsType);
	obs_sceneitem_set_bounds_alignment(item, state.boundsAlign);
	obs_sceneitem_set_bounds(item, &state.bounds);
	obs_sceneitem_set_crop(item, &state.crop);
}

void MotionCraftController::loadRecoveryMap(obs_data_t *data)
{
	recoveryTransforms.clear();
	if (!data)
		return;

	obs_data_array_t *arr = obs_data_get_array(data, "recovery_transforms");
	if (!arr)
		return;

	const size_t count = obs_data_array_count(arr);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *row = obs_data_array_item(arr, i);
		if (!row)
			continue;

		const char *rawKey = obs_data_get_string(row, "key");
		if (rawKey && *rawKey) {
			OrigState state{};
			state.pos.x = (float)obs_data_get_double(row, "pos_x");
			state.pos.y = (float)obs_data_get_double(row, "pos_y");
			state.scale.x = (float)obs_data_get_double(row, "scale_x");
			state.scale.y = (float)obs_data_get_double(row, "scale_y");
			state.rot = (float)obs_data_get_double(row, "rot");
			state.align = (uint32_t)obs_data_get_int(row, "align");
			state.boundsType = (obs_bounds_type)obs_data_get_int(row, "bounds_type");
			state.boundsAlign = (uint32_t)obs_data_get_int(row, "bounds_align");
			state.bounds.x = (float)obs_data_get_double(row, "bounds_x");
			state.bounds.y = (float)obs_data_get_double(row, "bounds_y");
			state.crop.left = (int)obs_data_get_int(row, "crop_left");
			state.crop.top = (int)obs_data_get_int(row, "crop_top");
			state.crop.right = (int)obs_data_get_int(row, "crop_right");
			state.crop.bottom = (int)obs_data_get_int(row, "crop_bottom");
			state.valid = true;
			recoveryTransforms.insert(QString::fromUtf8(rawKey), state);
		}

		obs_data_release(row);
	}

	obs_data_array_release(arr);
}

void MotionCraftController::saveRecoveryMap(obs_data_t *data)
{
	if (!data)
		return;

	obs_data_array_t *arr = obs_data_array_create();
	for (auto it = recoveryTransforms.constBegin(); it != recoveryTransforms.constEnd(); ++it) {
		const OrigState &state = it.value();
		if (!state.valid)
			continue;

		obs_data_t *row = obs_data_create();
		obs_data_set_string(row, "key", it.key().toUtf8().constData());
		obs_data_set_double(row, "pos_x", state.pos.x);
		obs_data_set_double(row, "pos_y", state.pos.y);
		obs_data_set_double(row, "scale_x", state.scale.x);
		obs_data_set_double(row, "scale_y", state.scale.y);
		obs_data_set_double(row, "rot", state.rot);
		obs_data_set_int(row, "align", (long long)state.align);
		obs_data_set_int(row, "bounds_type", (long long)state.boundsType);
		obs_data_set_int(row, "bounds_align", (long long)state.boundsAlign);
		obs_data_set_double(row, "bounds_x", state.bounds.x);
		obs_data_set_double(row, "bounds_y", state.bounds.y);
		obs_data_set_int(row, "crop_left", state.crop.left);
		obs_data_set_int(row, "crop_top", state.crop.top);
		obs_data_set_int(row, "crop_right", state.crop.right);
		obs_data_set_int(row, "crop_bottom", state.crop.bottom);
		obs_data_array_push_back(arr, row);
		obs_data_release(row);
	}

	obs_data_set_array(data, "recovery_transforms", arr);
	obs_data_array_release(arr);
}

void MotionCraftController::scheduleSettingsSave(int delayMs)
{
	if (shuttingDown || pendingSettingsSave)
		return;

	pendingSettingsSave = true;
	QTimer::singleShot(std::max(0, delayMs), this, [this]() {
		pendingSettingsSave = false;
		if (!shuttingDown)
			saveSettings();
	});
}

void MotionCraftController::restoreRecoveryIfNeeded()
{
	if (!recoveryActive)
		return;

	if (recoveryTransforms.isEmpty()) {
		clearRecoveryActive();
		return;
	}

	restoringRecovery = true;

	obs_frontend_source_list scenes{};
	obs_frontend_get_scenes(&scenes);

	struct Ctx {
		MotionCraftController *ctl = nullptr;
		int restored = 0;
		static void enumScene(obs_scene_t *scene, Ctx *ctx)
		{
			if (!scene || !ctx || !ctx->ctl)
				return;
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
					auto *ctx = static_cast<Ctx *>(param);
					if (!ctx || !ctx->ctl || !item)
						return true;

					const QString key = ctx->ctl->sceneItemKey(item);
					if (!key.isEmpty() && ctx->ctl->recoveryTransforms.contains(key)) {
						ctx->ctl->applySceneItemTransform(
							item, ctx->ctl->recoveryTransforms.value(key));
						ctx->restored++;
					} else {
						obs_source_t *src = obs_sceneitem_get_source(item);
						obs_scene_t *scene = obs_sceneitem_get_scene(item);
						obs_source_t *sceneSource = scene ? obs_scene_get_source(scene)
										  : nullptr;
						const char *sceneName = sceneSource ? obs_source_get_name(sceneSource)
										    : nullptr;
						const char *sourceName = src ? obs_source_get_name(src) : nullptr;
						if (sceneName && *sceneName && sourceName && *sourceName) {
							const QString prefix =
								QStringLiteral("%1::%2::")
									.arg(QString::fromUtf8(sceneName))
									.arg(QString::fromUtf8(sourceName));
							QString matched;
							for (auto it = ctx->ctl->recoveryTransforms.constBegin();
							     it != ctx->ctl->recoveryTransforms.constEnd(); ++it) {
								if (!it.key().startsWith(prefix))
									continue;
								if (!matched.isEmpty()) {
									matched.clear();
									break;
								}
								matched = it.key();
							}
							if (!matched.isEmpty()) {
								ctx->ctl->applySceneItemTransform(
									item,
									ctx->ctl->recoveryTransforms.value(matched));
								ctx->restored++;
							}
						}
					}

					obs_source_t *src = obs_sceneitem_get_source(item);
					obs_scene_t *subScene = src ? obs_scene_from_source(src) : nullptr;
					if (subScene)
						Ctx::enumScene(subScene, ctx);

					return true;
				},
				ctx);
		}
	};

	Ctx ctx{this};
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *sceneSource = scenes.sources.array[i];
		obs_scene_t *scene = sceneSource ? obs_scene_from_source(sceneSource) : nullptr;
		if (scene)
			Ctx::enumScene(scene, &ctx);
	}

	obs_frontend_source_list_free(&scenes);

	restoringRecovery = false;
	if (ctx.restored > 0)
		clearRecoveryActive();
}

void MotionCraftController::requestRecoveryRestore()
{
	if (shuttingDown || !recoveryActive)
		return;
	restoreRecoveryIfNeeded();
}

void MotionCraftController::frontendEventCallback(enum obs_frontend_event event, void *data)
{
	auto *ctl = static_cast<MotionCraftController *>(data);
	if (!ctl || ctl->shuttingDown)
		return;

	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED ||
	    event == OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED) {
		QTimer::singleShot(750, ctl, [ctl]() { ctl->requestRecoveryRestore(); });
		QTimer::singleShot(3000, ctl, [ctl]() { cleanup_legacy_marker_items_all_scenes(ctl->markerSource); });
	}

	/* Resume a wiggle that was running when OBS was closed, but only once the
	 * scenes are actually there, and only after the recovery restore above has
	 * had its turn - starting first would capture transforms that are still
	 * mid-restore and bake the last session's drift in as the original. */
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING && ctl->wiggleEnabled) {
		QTimer::singleShot(1500, ctl, [ctl]() {
			if (ctl->wiggleEnabled && ctl->pluginEnabled && !ctl->shuttingDown)
				ctl->setWiggleEnabled(true);
		});
	}
}

void MotionCraftController::markRecoveryActive()
{
	if (recoveryActive)
		return;
	recoveryActive = true;
	if (!shuttingDown)
		saveSettings();
}

void MotionCraftController::clearRecoveryActive()
{
	if (!recoveryActive)
		return;
	recoveryActive = false;
	if (!shuttingDown)
		saveSettings();
}

static void ensure_parent_dir_exists(const QString &filePath)
{
	if (filePath.isEmpty())
		return;
	QFileInfo fi(filePath);
	const QString dir = fi.absolutePath();
	if (dir.isEmpty())
		return;
	QByteArray dirUtf8 = dir.toUtf8();
	os_mkdirs(dirUtf8.constData());
}

void MotionCraftController::initialize()
{
	loadSettings();
	rebuildTriggersFromSettings();
	obs_frontend_add_event_callback(frontendEventCallback, this);
	obs_add_tick_callback(videoTickCallback, this);
	tickCallbackAdded = true;
	QTimer::singleShot(1500, this, [this]() { installHooks(); });
	QTimer::singleShot(3000, this, [this]() { requestRecoveryRestore(); });
	QTimer::singleShot(5000, this, [this]() { cleanup_legacy_marker_items_all_scenes(markerSource); });
}

void MotionCraftController::shutdown()
{
	shuttingDown = true;
	/* Must come first: obs_remove_tick_callback blocks until any in-flight tick
	 * returns, so after this the graphics thread can no longer touch our state
	 * while the teardown below dismantles it. */
	if (tickCallbackAdded) {
		obs_remove_tick_callback(videoTickCallback, this);
		tickCallbackAdded = false;
	}
	zoomActive.store(false, std::memory_order_release);
	tickingWanted.store(false, std::memory_order_release);
	obs_frontend_remove_event_callback(frontendEventCallback, this);
	uninstallHooks();
	ensureTicking(false);

	obs_source_t *staleRef = nullptr;
	{
		std::lock_guard<std::mutex> lock(inputMutex);
		staleRef = input.sceneRef;
		input.sceneRef = nullptr;
	}
	if (staleRef)
		obs_source_release(staleRef);
	if (dialog)
		dialog->close();
}

void MotionCraftController::showDialog()
{
	if (!dialog) {
		dialog = new MotionCraftDialog(reinterpret_cast<QWidget *>(obs_frontend_get_main_window()));
		dialog->setAttribute(Qt::WA_DeleteOnClose, true);
		connect(dialog.data(), &QObject::destroyed, this, [this]() { dialog = nullptr; });
	}
	dialog->show();
	dialog->raise();
	dialog->activateWindow();
}

void MotionCraftController::loadSettings()
{
	screenKey.clear();

	/* Times ramp with the zoom so that stepping between adjacent levels costs
	 * roughly the same wall clock wherever you are on the ladder. */
	static const struct {
		double zoom;
		int inMs;
		int outMs;
	} kLevelDefaults[kZoomLevelCount] = {
		{1.25, 300, 300}, {1.50, 500, 500}, {2.00, 800, 800}, {3.00, 1200, 1200}, {4.00, 1600, 1600},
	};
	for (int i = 0; i < kZoomLevelCount; i++) {
		zoomLevels[i] = ZoomLevel{};
		zoomLevels[i].zoom = kLevelDefaults[i].zoom;
		zoomLevels[i].inMs = kLevelDefaults[i].inMs;
		zoomLevels[i].outMs = kLevelDefaults[i].outMs;
	}

	hotkeysEnabled = true;
	followMouse = true;
	followMouseRuntimeEnabled = true;
	followSpeed = 8.0;
	centerCursorUntilEdge = true;
	mouseIdleTimeoutMs = 0;
	portraitCover = true;
	showCursorMarker = false;
	markerOnlyOnClick = true;
	markerColor = 0xFFFF0000;
	markerSize = 26;
	markerThickness = 4;
	debug = false;

	wiggleEnabled = false;
	wigglePositionPx = 2.0;
	wiggleRotationDeg = 0.3;
	wiggleScalePct = 0.5;
	wiggleSpeedMin = 1.0;
	wiggleSpeedMax = 2.0;
	wiggleSeed = 1234;

	const QString p = configPath();
	if (p.isEmpty())
		return;

	QByteArray pUtf8 = p.toUtf8();
	obs_data_t *data = obs_data_create_from_json_file_safe(pUtf8.constData(), "bak");
	if (!data) {
		/* First run after the Zoominator rename: adopt the old config rather
		 * than silently starting from defaults. It is only read - the next
		 * save writes to the MotionCraft path and the old file is left alone. */
		const QString legacy = legacyConfigPath();
		if (legacy.isEmpty())
			return;
		QByteArray legacyUtf8 = legacy.toUtf8();
		data = obs_data_create_from_json_file_safe(legacyUtf8.constData(), "bak");
		if (!data)
			return;
		blog(LOG_INFO, "[MotionCraft] Adopted existing Zoominator settings from: %s", legacyUtf8.constData());
	}

	auto getStr = [&](const char *key) -> QString {
		const char *v = obs_data_get_string(data, key);
		return v ? QString::fromUtf8(v) : QString();
	};

	screenKey = getStr("screen_key");
	if (screenKey.isEmpty())
		screenKey = getStr("source_name");
	followToggleHotkeySequence = getStr("follow_toggle_hotkey");
	pluginToggleHotkeySequence = getStr("plugin_toggle_hotkey");

	QKeySequence followSeq(followToggleHotkeySequence);
	if (!followSeq.isEmpty()) {
		const QKeyCombination kc = followSeq[0];
		const auto mods = kc.keyboardModifiers();
		const int key = int(kc.key());
		followToggleHotkeyVk = qtKeyToVk(key);
		followToggleModCtrl = mods.testFlag(Qt::ControlModifier);
		followToggleModAlt = mods.testFlag(Qt::AltModifier);
		followToggleModShift = mods.testFlag(Qt::ShiftModifier);
		followToggleModWin = mods.testFlag(Qt::MetaModifier);

#ifdef _WIN32
		const bool keyIsModifier = (followToggleHotkeyVk == VK_CONTROL || followToggleHotkeyVk == VK_LCONTROL ||
					    followToggleHotkeyVk == VK_RCONTROL || followToggleHotkeyVk == VK_MENU ||
					    followToggleHotkeyVk == VK_LMENU || followToggleHotkeyVk == VK_RMENU ||
					    followToggleHotkeyVk == VK_SHIFT || followToggleHotkeyVk == VK_LSHIFT ||
					    followToggleHotkeyVk == VK_RSHIFT || followToggleHotkeyVk == VK_LWIN ||
					    followToggleHotkeyVk == VK_RWIN);
#elif defined(__APPLE__)
		const bool keyIsModifier =
			(followToggleHotkeyVk == kVK_Control || followToggleHotkeyVk == kVK_RightControl ||
			 followToggleHotkeyVk == kVK_Option || followToggleHotkeyVk == kVK_RightOption ||
			 followToggleHotkeyVk == kVK_Shift || followToggleHotkeyVk == kVK_RightShift ||
			 followToggleHotkeyVk == kVK_Command || followToggleHotkeyVk == kVK_RightCommand);
#elif defined(__linux__)
		const bool keyIsModifier = (followToggleHotkeyVk == XK_Control_L ||
					    followToggleHotkeyVk == XK_Control_R || followToggleHotkeyVk == XK_Alt_L ||
					    followToggleHotkeyVk == XK_Alt_R || followToggleHotkeyVk == XK_Shift_L ||
					    followToggleHotkeyVk == XK_Shift_R || followToggleHotkeyVk == XK_Super_L ||
					    followToggleHotkeyVk == XK_Super_R);
#else
		const bool keyIsModifier = false;
#endif

		if (keyIsModifier && mods == Qt::NoModifier) {
#ifdef _WIN32
			followToggleModCtrl = (followToggleHotkeyVk == VK_CONTROL ||
					       followToggleHotkeyVk == VK_LCONTROL ||
					       followToggleHotkeyVk == VK_RCONTROL);
			followToggleModAlt = (followToggleHotkeyVk == VK_MENU || followToggleHotkeyVk == VK_LMENU ||
					      followToggleHotkeyVk == VK_RMENU);
			followToggleModShift = (followToggleHotkeyVk == VK_SHIFT || followToggleHotkeyVk == VK_LSHIFT ||
						followToggleHotkeyVk == VK_RSHIFT);
			followToggleModWin = (followToggleHotkeyVk == VK_LWIN || followToggleHotkeyVk == VK_RWIN);
#elif defined(__APPLE__)
			followToggleModCtrl =
				(followToggleHotkeyVk == kVK_Control || followToggleHotkeyVk == kVK_RightControl);
			followToggleModAlt =
				(followToggleHotkeyVk == kVK_Option || followToggleHotkeyVk == kVK_RightOption);
			followToggleModShift =
				(followToggleHotkeyVk == kVK_Shift || followToggleHotkeyVk == kVK_RightShift);
			followToggleModWin =
				(followToggleHotkeyVk == kVK_Command || followToggleHotkeyVk == kVK_RightCommand);
#elif defined(__linux__)
			followToggleModCtrl =
				(followToggleHotkeyVk == XK_Control_L || followToggleHotkeyVk == XK_Control_R);
			followToggleModAlt = (followToggleHotkeyVk == XK_Alt_L || followToggleHotkeyVk == XK_Alt_R);
			followToggleModShift =
				(followToggleHotkeyVk == XK_Shift_L || followToggleHotkeyVk == XK_Shift_R);
			followToggleModWin = (followToggleHotkeyVk == XK_Super_L || followToggleHotkeyVk == XK_Super_R);
#else
			followToggleHotkeyVk = 0;
#endif
		}

		followToggleHkValid = (followToggleHotkeyVk != 0) &&
				      (followToggleModCtrl || followToggleModAlt || followToggleModShift ||
				       followToggleModWin || key != 0);
	}

	obs_data_array_t *levelArr = obs_data_get_array(data, "zoom_levels");
	if (levelArr) {
		const size_t levelCount = obs_data_array_count(levelArr);
		for (size_t i = 0; i < levelCount && i < (size_t)kZoomLevelCount; i++) {
			obs_data_t *lvItem = obs_data_array_item(levelArr, i);
			if (!lvItem)
				continue;
			ZoomLevel &lv = zoomLevels[i];
			if (obs_data_has_user_value(lvItem, "zoom"))
				lv.zoom = obs_data_get_double(lvItem, "zoom");
			if (obs_data_has_user_value(lvItem, "in_ms"))
				lv.inMs = (int)obs_data_get_int(lvItem, "in_ms");
			if (obs_data_has_user_value(lvItem, "out_ms"))
				lv.outMs = (int)obs_data_get_int(lvItem, "out_ms");
			const char *lvHk = obs_data_get_string(lvItem, "hotkey");
			lv.hotkey = lvHk ? QString::fromUtf8(lvHk) : QString();
			obs_data_release(lvItem);
		}
		obs_data_array_release(levelArr);
	} else if (obs_data_has_user_value(data, "zoom_factor") || obs_data_has_user_value(data, "hotkey")) {
		/* Config written by a build that had a single zoom trigger. Carry it
		 * over as level 1 so an existing setup keeps working; the remaining
		 * levels stay at their defaults with no hotkey bound. The old Hold
		 * behaviour has no equivalent - level keys toggle. */
		ZoomLevel &lv = zoomLevels[0];
		if (obs_data_has_user_value(data, "zoom_factor"))
			lv.zoom = obs_data_get_double(data, "zoom_factor");
		if (obs_data_has_user_value(data, "anim_in_ms"))
			lv.inMs = (int)obs_data_get_int(data, "anim_in_ms");
		if (obs_data_has_user_value(data, "anim_out_ms"))
			lv.outMs = (int)obs_data_get_int(data, "anim_out_ms");
		if (obs_data_get_string(data, "trigger_type") &&
		    strcmp(obs_data_get_string(data, "trigger_type"), "mouse") != 0) {
			const char *oldHk = obs_data_get_string(data, "hotkey");
			lv.hotkey = oldHk ? QString::fromUtf8(oldHk) : QString();
		}
		blog(LOG_INFO, "[MotionCraft] Migrated the single zoom trigger to zoom level 1.");
	}

	for (int i = 0; i < kZoomLevelCount; i++) {
		ZoomLevel &lv = zoomLevels[i];
		lv.zoom = clampd(lv.zoom, 1.0, 8.0);
		lv.inMs = std::clamp(lv.inMs, 0, 10000);
		lv.outMs = std::clamp(lv.outMs, 0, 10000);
	}

	if (obs_data_has_user_value(data, "hotkeys_enabled"))
		hotkeysEnabled = obs_data_get_bool(data, "hotkeys_enabled");

	if (obs_data_has_user_value(data, "follow_mouse"))
		followMouse = obs_data_get_bool(data, "follow_mouse");
	followMouseRuntimeEnabled = true;
	if (obs_data_has_user_value(data, "follow_speed"))
		followSpeed = obs_data_get_double(data, "follow_speed");
	if (followSpeed <= 0.1)
		followSpeed = 8.0;
	if (obs_data_has_user_value(data, "center_cursor_until_edge"))
		centerCursorUntilEdge = obs_data_get_bool(data, "center_cursor_until_edge");
	if (obs_data_has_user_value(data, "mouse_idle_timeout_ms"))
		mouseIdleTimeoutMs = (int)obs_data_get_int(data, "mouse_idle_timeout_ms");
	mouseIdleTimeoutMs = std::clamp(mouseIdleTimeoutMs, 0, 60000);

	if (obs_data_has_user_value(data, "portrait_cover"))
		portraitCover = obs_data_get_bool(data, "portrait_cover");
	if (obs_data_has_user_value(data, "show_cursor_marker"))
		showCursorMarker = obs_data_get_bool(data, "show_cursor_marker");
	// Continuous cursor tracking was inaccurate on captures whose aspect ratio
	// differs from the OBS canvas. The marker is intentionally click-only.
	markerOnlyOnClick = true;
	if (obs_data_has_user_value(data, "marker_color"))
		markerColor = (uint32_t)obs_data_get_int(data, "marker_color");
	if (obs_data_has_user_value(data, "marker_size"))
		markerSize = (int)obs_data_get_int(data, "marker_size");
	if (obs_data_has_user_value(data, "marker_thickness"))
		markerThickness = (int)obs_data_get_int(data, "marker_thickness");

	if (obs_data_has_user_value(data, "debug"))
		debug = obs_data_get_bool(data, "debug");

	if (obs_data_has_user_value(data, "wiggle_enabled"))
		wiggleEnabled = obs_data_get_bool(data, "wiggle_enabled");
	if (obs_data_has_user_value(data, "wiggle_position_px"))
		wigglePositionPx = obs_data_get_double(data, "wiggle_position_px");
	if (obs_data_has_user_value(data, "wiggle_rotation_deg"))
		wiggleRotationDeg = obs_data_get_double(data, "wiggle_rotation_deg");
	if (obs_data_has_user_value(data, "wiggle_scale_pct"))
		wiggleScalePct = obs_data_get_double(data, "wiggle_scale_pct");
	if (obs_data_has_user_value(data, "wiggle_speed_min"))
		wiggleSpeedMin = obs_data_get_double(data, "wiggle_speed_min");
	if (obs_data_has_user_value(data, "wiggle_speed_max"))
		wiggleSpeedMax = obs_data_get_double(data, "wiggle_speed_max");
	if (obs_data_has_user_value(data, "wiggle_seed"))
		wiggleSeed = (int)obs_data_get_int(data, "wiggle_seed");

	wigglePositionPx = clampd(wigglePositionPx, 0.0, kWigglePositionMaxPx);
	wiggleRotationDeg = clampd(wiggleRotationDeg, 0.0, kWiggleRotationMaxDeg);
	wiggleScalePct = clampd(wiggleScalePct, 0.0, kWiggleScaleMaxPct);
	wiggleSpeedMin = clampd(wiggleSpeedMin, 0.0, kWiggleSpeedMax);
	wiggleSpeedMax = clampd(wiggleSpeedMax, 0.0, kWiggleSpeedMax);
	if (wiggleSpeedMax < wiggleSpeedMin)
		std::swap(wiggleSpeedMin, wiggleSpeedMax);

	includedSources.clear();
	obs_data_array_t *incArr = obs_data_get_array(data, "included_sources");
	if (incArr) {
		const size_t incCount = obs_data_array_count(incArr);
		for (size_t i = 0; i < incCount; i++) {
			obs_data_t *incItem = obs_data_array_item(incArr, i);
			if (incItem) {
				const char *incName = obs_data_get_string(incItem, "name");
				if (incName && *incName)
					includedSources.insert(QString::fromUtf8(incName));
				obs_data_release(incItem);
			}
		}
		obs_data_array_release(incArr);
	}

	recoveryActive = obs_data_get_bool(data, "recovery_active");
	loadRecoveryMap(data);

	obs_data_release(data);

	logi(debug, "[MotionCraft] Loaded settings from: %s", pUtf8.constData());

	rebuildTriggersFromSettings();
	emit settingsChanged();
}

void MotionCraftController::notifySettingsChanged()
{
	emit settingsChanged();
}

void MotionCraftController::saveSettings()
{
	const QString p = configPath();
	if (p.isEmpty())
		return;

	ensure_parent_dir_exists(p);

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "screen_key", screenKey.toUtf8().constData());
	obs_data_set_string(data, "follow_toggle_hotkey", followToggleHotkeySequence.toUtf8().constData());
	obs_data_set_string(data, "plugin_toggle_hotkey", pluginToggleHotkeySequence.toUtf8().constData());

	obs_data_array_t *levelArr = obs_data_array_create();
	for (const ZoomLevel &lv : zoomLevels) {
		obs_data_t *lvItem = obs_data_create();
		obs_data_set_double(lvItem, "zoom", lv.zoom);
		obs_data_set_int(lvItem, "in_ms", lv.inMs);
		obs_data_set_int(lvItem, "out_ms", lv.outMs);
		obs_data_set_string(lvItem, "hotkey", lv.hotkey.toUtf8().constData());
		obs_data_array_push_back(levelArr, lvItem);
		obs_data_release(lvItem);
	}
	obs_data_set_array(data, "zoom_levels", levelArr);
	obs_data_array_release(levelArr);

	obs_data_set_bool(data, "hotkeys_enabled", hotkeysEnabled);
	obs_data_set_bool(data, "follow_mouse", followMouse);
	followMouseRuntimeEnabled = true;
	obs_data_set_double(data, "follow_speed", followSpeed);
	obs_data_set_bool(data, "center_cursor_until_edge", centerCursorUntilEdge);
	obs_data_set_int(data, "mouse_idle_timeout_ms", mouseIdleTimeoutMs);
	obs_data_set_bool(data, "portrait_cover", portraitCover);
	obs_data_set_bool(data, "show_cursor_marker", showCursorMarker);
	obs_data_set_bool(data, "marker_only_on_click", true);
	obs_data_set_int(data, "marker_color", (long long)markerColor);
	obs_data_set_int(data, "marker_size", markerSize);
	obs_data_set_int(data, "marker_thickness", markerThickness);
	obs_data_set_bool(data, "debug", debug);

	obs_data_set_bool(data, "wiggle_enabled", wiggleEnabled);
	obs_data_set_double(data, "wiggle_position_px", wigglePositionPx);
	obs_data_set_double(data, "wiggle_rotation_deg", wiggleRotationDeg);
	obs_data_set_double(data, "wiggle_scale_pct", wiggleScalePct);
	obs_data_set_double(data, "wiggle_speed_min", wiggleSpeedMin);
	obs_data_set_double(data, "wiggle_speed_max", wiggleSpeedMax);
	obs_data_set_int(data, "wiggle_seed", wiggleSeed);

	obs_data_set_bool(data, "recovery_active", recoveryActive);
	saveRecoveryMap(data);

	obs_data_array_t *incArr = obs_data_array_create();
	for (const QString &incName : includedSources) {
		obs_data_t *incItem = obs_data_create();
		obs_data_set_string(incItem, "name", incName.toUtf8().constData());
		obs_data_array_push_back(incArr, incItem);
		obs_data_release(incItem);
	}
	obs_data_set_array(data, "included_sources", incArr);
	obs_data_array_release(incArr);

	QByteArray pUtf8 = p.toUtf8();
	obs_data_save_json_safe(data, pUtf8.constData(), "tmp", "bak");
	obs_data_release(data);

	logi(debug, "[MotionCraft] Saved settings to: %s", pUtf8.constData());

	rebuildTriggersFromSettings();
	emit settingsChanged();
}

void MotionCraftController::rebuildRuntimeHooks()
{
	rebuildTriggersFromSettings();

	/* Turning the hotkeys off while zoomed would otherwise strand the scene
	 * there with no key left to bring it back. Ride it out normally so the
	 * transforms are restored the same way they always are. */
	if (!hotkeysEnabled && requestedLevel != 0)
		requestLevel(0);

	uninstallHooks();
	installHooks();
}

void MotionCraftController::ensureTicking(bool on)
{
	if (on) {
		if (!tickTimer.isActive())
			tickTimer.start();
	} else {
		if (tickTimer.isActive())
			tickTimer.stop();
	}
}

bool MotionCraftController::wiggleShaping() const
{
	if (!wiggleRunning.load(std::memory_order_acquire))
		return false;
	return wigglePositionPx > 0.0 || wiggleRotationDeg > 0.0 || wiggleScalePct > 0.0;
}

/* How much bigger every item has to be drawn so the drift cannot pull the
 * canvas background into view.
 *
 * Each term answers "by what factor must a canvas-shaped rectangle grow so it
 * still covers the canvas after this component's worst case?":
 *
 *   position  a shift of up to p in either direction needs p of overhang on
 *             each edge, i.e. 2p more than the canvas on each axis;
 *   rotation  a rectangle turned by th covers an axis-aligned one only if its
 *             half-extents beat that one's support in both edge normals,
 *             which gives (W|cos| + H|sin|)/W and (H|cos| + W|sin|)/H;
 *   scale     the wiggle scales by 1 +/- s, so the shrink is what has to be
 *             paid for: 1/(1 - s), not 1 + s.
 *
 * They compound, and 2% is added on top for the residual an item that is not
 * centred or not canvas-shaped brings with it. This is a margin, not a proof -
 * the per-frame framing clamp in applyZoomToScene is what actually guarantees
 * no background is exposed, and it can only do that if there is slack here to
 * work with. */
double MotionCraftController::wiggleSafetyScale() const
{
	if (!wiggleShaping())
		return 1.0;

	obs_video_info ovi{};
	const bool haveVi = obs_get_video_info(&ovi);
	const double cw = haveVi && ovi.base_width > 0 ? (double)ovi.base_width : 1920.0;
	const double ch = haveVi && ovi.base_height > 0 ? (double)ovi.base_height : 1080.0;

	const double p = clampd(wigglePositionPx, 0.0, kWigglePositionMaxPx);
	const double posScale = std::max((cw + 2.0 * p) / cw, (ch + 2.0 * p) / ch);

	const double th = clampd(wiggleRotationDeg, 0.0, kWiggleRotationMaxDeg) * 3.14159265358979323846 / 180.0;
	const double c = std::cos(th);
	const double s = std::sin(th);
	const double rotScale = std::max((cw * c + ch * s) / cw, (ch * c + cw * s) / ch);

	const double sc = clampd(wiggleScalePct, 0.0, kWiggleScaleMaxPct) / 100.0;
	const double scaleScale = 1.0 / std::max(0.01, 1.0 - sc);

	return posScale * rotScale * scaleScale * 1.02;
}

/* Graphics thread only. Draws a fresh speed every kWiggleSpeedSampleSec and
 * glides onto it, which is what turns a steady drift into something that
 * hesitates and hurries the way a real hand does. Phase is integrated from the
 * result, never recomputed as elapsed * speed, so changing speed bends the
 * motion instead of jumping it. */
void MotionCraftController::updateWiggleSpeed(double seconds)
{
	const double lo = clampd(std::min(wiggleSpeedMin, wiggleSpeedMax), 0.0, kWiggleSpeedMax);
	const double hi = clampd(std::max(wiggleSpeedMin, wiggleSpeedMax), 0.0, kWiggleSpeedMax);

	if (hi - lo <= 1e-6) {
		wiggleSpeedTarget = lo;
		wiggleSpeedCurrent = lo;
	} else {
		wiggleSpeedTimer -= seconds;
		if (wiggleSpeedTimer <= 0.0) {
			wiggleSpeedTimer += kWiggleSpeedSampleSec;
			if (wiggleSpeedTimer <= 0.0)
				wiggleSpeedTimer = kWiggleSpeedSampleSec;
			wiggleSpeedRng = wiggle_hash(wiggleSpeedRng + 0x9e3779b9u);
			const double u = (double)wiggleSpeedRng / 4294967295.0;
			wiggleSpeedTarget = lo + u * (hi - lo);
		}
		const double blend = 1.0 - std::exp(-seconds / kWiggleSpeedGlideSec);
		wiggleSpeedCurrent += (wiggleSpeedTarget - wiggleSpeedCurrent) * blend;
	}

	wigglePhase += seconds * wiggleSpeedCurrent;
}

double MotionCraftController::levelZoom(int level) const
{
	if (level < 1 || level > kZoomLevelCount)
		return 1.0;
	const double z = zoomLevels[level - 1].zoom;
	return (z > 1.0) ? z : 1.0;
}

/* Position of a zoom value on the in- or out-timeline, whose origin is the
 * unzoomed state. Levels sit at the coordinates the user configured; a zoom
 * that falls between two levels - which is where an interrupted transition
 * starts - is interpolated from the pair bracketing it. Transition duration is
 * then just the distance between two coordinates, which gives |in(B) - in(A)|
 * for a plain A -> B zoom in and in(B) for a zoom in from rest. */
double MotionCraftController::timelineMsForZoom(double z, bool zoomingIn) const
{
	struct Point {
		double zoom;
		double ms;
	};

	Point points[kZoomLevelCount + 1];
	int count = 0;
	points[count++] = {1.0, 0.0};
	for (const ZoomLevel &lv : zoomLevels) {
		if (lv.zoom > 1.0)
			points[count++] = {lv.zoom, (double)(zoomingIn ? lv.inMs : lv.outMs)};
	}
	std::sort(points, points + count, [](const Point &a, const Point &b) { return a.zoom < b.zoom; });

	if (z <= points[0].zoom)
		return points[0].ms;

	for (int i = 1; i < count; i++) {
		if (z > points[i].zoom)
			continue;
		const double span = points[i].zoom - points[i - 1].zoom;
		if (span <= 1e-9)
			return points[i].ms;
		const double f = (z - points[i - 1].zoom) / span;
		return points[i - 1].ms + (points[i].ms - points[i - 1].ms) * f;
	}

	return points[count - 1].ms;
}

/* Graphics thread only: seeds a transition from wherever the zoom currently is,
 * at whatever speed it currently has. */
void MotionCraftController::beginSegment(int level)
{
	const double zTarget = levelZoom(level);
	const double zNow = currentZoom;
	const bool zoomingIn = (zTarget > zNow);
	const double durMs = std::fabs(timelineMsForZoom(zTarget, zoomingIn) - timelineMsForZoom(zNow, zoomingIn));

	segZ0 = zNow;
	segZ1 = zTarget;
	segV0 = currentZoomVel;
	segU = 0.0;
	segDurSec = durMs / 1000.0;

	if (durMs < 1.0) {
		/* Both endpoints share a timeline coordinate, so there is no ramp to
		 * run. Snap, and drop any carried velocity with it. */
		currentZoom = zTarget;
		currentZoomVel = 0.0;
		segmentRunning.store(false, std::memory_order_relaxed);
		return;
	}

	segmentRunning.store(true, std::memory_order_relaxed);
}

void MotionCraftController::activateLevel(int level)
{
	/* Gated here rather than in each platform's hook so no backend can drift.
	 * requestLevel() is deliberately not gated: it is also the path that
	 * unwinds a zoom when the hotkeys are switched off. */
	if (!pluginEnabled || !hotkeysEnabled || !hotkeyFocusAllows() || level < 1 || level > kZoomLevelCount)
		return;

	/* The graphics thread flags a completed unzoom and the Qt timer does the
	 * actual teardown up to a frame later. Draining it here as well as in
	 * requestLevel matters: requestedLevel is read below to decide the toggle,
	 * and the teardown is what resets it. */
	if (pendingFinish.exchange(false))
		finishZoomOnMainThread();

	/* A level configured at 1.0 is not a zoom, so treat its key as "go back to
	 * unzoomed" rather than as a level you can sit at. */
	const int pressed = (levelZoom(level) > 1.0) ? level : 0;
	logi(debug, "[MotionCraft] Level key %d pressed while at level %d", level, requestedLevel);
	requestLevel((requestedLevel == pressed) ? 0 : pressed);
}

/* The plugin's own on/off, reached from the toggle hotkey or the dialog.
 *
 * Switching off has to leave the scene exactly as it was found, so it does not
 * simply stop driving: it asks the graphics thread to abort, which hands back
 * through the same pendingFinish path a zoom uses when it ends by itself. That
 * path is what restores every transform and clears the recovery marker. One
 * frame later - under 17 ms - the scene is back to normal.
 *
 * Hooks are then rebuilt rather than torn down, because the toggle key itself
 * still has to be heard. Everything else stops being listened for. */
void MotionCraftController::setPluginEnabled(bool on)
{
	if (pluginEnabled == on)
		return;

	pluginEnabled = on;

	if (!on) {
		wiggleRunning.store(false, std::memory_order_release);
		requestedLevel = 0;
		targetLevel.store(0, std::memory_order_relaxed);
		retargetRequested.store(false, std::memory_order_relaxed);

		if (zoomActive.load(std::memory_order_acquire)) {
			abortRequested.store(true, std::memory_order_release);
		} else if (tickingWanted.load(std::memory_order_acquire)) {
			/* Ticking but nothing captured yet, so there is nothing for
			 * the graphics thread to hand back - stop it here. */
			tickingWanted.store(false, std::memory_order_release);
			ensureTicking(false);
			resetState();
		}
	}

	blog(LOG_INFO, "[MotionCraft] Plugin %s", on ? "ENABLED" : "DISABLED");

	/* Deferred to the next turn of the event loop because this is normally
	 * reached from inside the keyboard hook, and Windows drops a low-level
	 * hook whose callback overruns LowLevelHooksTimeout. Rebuilding hooks and
	 * repainting the dialog are both far too much to do in that window. The
	 * hook callback runs on this thread, so a zero-delay timer lands as soon
	 * as it returns. */
	QTimer::singleShot(0, this, [this]() {
		if (shuttingDown)
			return;
		uninstallHooks();
		installHooks();
		emit settingsChanged();
	});
}

void MotionCraftController::togglePluginEnabled()
{
	/* Reached only from a hotkey, so it obeys the same focus policy as the
	 * others. hotkeysEnabled is deliberately not checked: that switch governs
	 * the zoom and follow keys, and being unable to turn the plugin back on
	 * because its own key had been filed under them would be a trap. */
	if (!hotkeyFocusAllows())
		return;
	setPluginEnabled(!pluginEnabled);
}

/* Wiggle's equivalent of requestLevel. It does not touch the zoom at all: it
 * only keeps the tick alive at rest, which is the one thing the zoom loop would
 * otherwise never do. Turning it off leaves the tick running until the zoom
 * (if any) finishes on its own, and videoTick then tears everything down
 * through the normal path. */
void MotionCraftController::setWiggleEnabled(bool on)
{
	wiggleEnabled = on;

	if (!on || !pluginEnabled) {
		wiggleRunning.store(false, std::memory_order_release);
		return;
	}

	if (pendingFinish.exchange(false))
		finishZoomOnMainThread();

	const bool wasIdle = !tickingWanted.load(std::memory_order_acquire);
	if (wasIdle) {
		markRecoveryActive();
		lastTickMs = 0;
		pendingFinish.store(false, std::memory_order_release);
		wigglePhase = 0.0;
		wiggleSpeedTimer = 0.0;
		wiggleSpeedCurrent = clampd(std::min(wiggleSpeedMin, wiggleSpeedMax), 0.0, kWiggleSpeedMax);
		wiggleSpeedTarget = wiggleSpeedCurrent;
		wiggleSpeedRng = (uint32_t)wiggleSeed;
	}

	wiggleRunning.store(true, std::memory_order_release);
	tickingWanted.store(true, std::memory_order_release);
	ensureTicking(true);

	if (wasIdle)
		onTick();
}

void MotionCraftController::requestLevel(int next)
{
	/* A level requested inside the teardown gap would otherwise re-capture the
	 * still-normalized transforms as if they were the originals, permanently
	 * losing the item's real bounds type and alignment. Finish, then start clean. */
	if (pendingFinish.exchange(false))
		finishZoomOnMainThread();

	const int previous = requestedLevel;
	requestedLevel = next;

	logi(debug, "[MotionCraft] Zoom level %d -> %d", previous, next);

	if (previous == 0 && next == 0)
		return;

	if (previous == 0 && next != 0) {
		/* Leaving rest. Everything the follow-mouse anchor derives from has to
		 * start clean here and only here - resetting it on every level change
		 * would yank the focal point mid-zoom. */
		markRecoveryActive();
		lastTickMs = 0;
		pendingFinish.store(false, std::memory_order_release);
		followHasPos = false;
		lastCursorSampleValid = false;
		lastCursorMovementMs = 0;
		mouseTrackingIdle = false;
		targetHasPos = false;
	}

	if (next == 0) {
		markerClickFlashStartMs = 0;
		markerClickFlashHoldUntilMs = 0;
		markerClickFlashFadeOutEndMs = 0;
		markerClickHasPos = false;
	}

	targetLevel.store(next, std::memory_order_relaxed);
	retargetRequested.store(true, std::memory_order_release);
	tickingWanted.store(true, std::memory_order_release);
	ensureTicking(true);

	if (previous == 0 && next != 0) {
		/* Sample immediately so the first rendered frame after the key already
		 * has a scene and a cursor position, rather than waiting for the Qt
		 * timer. */
		onTick();
	}
}

void MotionCraftController::resetState()
{
	zoomActive.store(false, std::memory_order_release);
	tickingWanted.store(false, std::memory_order_release);
	pendingFinish.store(false, std::memory_order_release);
	wiggleRunning.store(false, std::memory_order_release);
	abortRequested.store(false, std::memory_order_release);
	wigglePhase = 0.0;
	wiggleSpeedCurrent = 0.0;
	wiggleSpeedTarget = 0.0;
	wiggleSpeedTimer = 0.0;
	wiggleSafety = 1.0;
	requestedLevel = 0;
	targetLevel.store(0, std::memory_order_relaxed);
	retargetRequested.store(false, std::memory_order_relaxed);
	segmentRunning.store(false, std::memory_order_relaxed);
	segZ0 = 1.0;
	segZ1 = 1.0;
	segV0 = 0.0;
	segDurSec = 0.0;
	segU = 1.0;
	currentZoom = 1.0;
	currentZoomVel = 0.0;
	followHasPos = false;
	lastCursorSampleValid = false;
	lastCursorMovementMs = 0;
	mouseTrackingIdle = false;
	targetHasPos = false;
	tickDeltaSeconds = 1.0 / 60.0;
	lastTickMs = 0;
	lastTransformApplyMs = 0;
	lastFollowAnchorValid = false;
	sceneItems.clear();
	sceneContentBoundsValid = false;
	sceneContentMin = {};
	sceneContentMax = {};
	markerCurrentOpacity = -1;
	markerClickFlashStartMs = 0;
	markerClickFlashHoldUntilMs = 0;
	markerClickFlashFadeOutEndMs = 0;
	markerClickHasPos = false;
	obs_source_t *sceneSource = obs_frontend_get_current_scene();
	if (sceneSource) {
		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		if (scene)
			hideMarkerInScene(scene);
		obs_source_release(sceneSource);
	}
}

static QString make_screen_key_from_rect(int x, int y, int w, int h)
{
	return QStringLiteral("%1,%2,%3,%4").arg(x).arg(y).arg(w).arg(h);
}

void MotionCraftController::enumerateTargetItemsInCurrentScene(std::vector<obs_sceneitem_t *> &items) const
{
	items.clear();

	obs_source_t *sceneSource = obs_frontend_get_current_scene();
	if (!sceneSource)
		return;

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_source_release(sceneSource);
	if (!scene)
		return;

	struct Ctx {
		std::vector<obs_sceneitem_t *> *items = nullptr;
		const QSet<QString> *included = nullptr;

		static bool is_marker_item(obs_sceneitem_t *item)
		{
			if (!item)
				return true;
			obs_source_t *src = obs_sceneitem_get_source(item);
			if (!src)
				return true;
			if (src == MotionCraftController::instance().markerSource)
				return true;
			const char *srcName = obs_source_get_name(src);
			const char *srcId = obs_source_get_id(src);
			if (srcId && QString::fromUtf8(srcId) == QString::fromUtf8(kMotionCraftMarkerSourceId))
				return true;
			return source_name_starts_with(srcName, kMotionCraftMarkerSourceName);
		}

		static void enum_scene(obs_scene_t *scene, Ctx *ctx)
		{
			if (!scene || !ctx || !ctx->items)
				return;
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
					auto *ctx = static_cast<Ctx *>(param);
					if (!ctx || !ctx->items || !item)
						return true;
					if (is_marker_item(item))
						return true;

					obs_source_t *src = obs_sceneitem_get_source(item);
					if (!src)
						return true;

					if (obs_scene_t *subScene = obs_scene_from_source(src)) {
						enum_scene(subScene, ctx);
						return true;
					}

					if (!ctx->included || ctx->included->isEmpty())
						return true;

					const char *srcName = obs_source_get_name(src);
					if (!srcName || !ctx->included->contains(QString::fromUtf8(srcName)))
						return true;

					ctx->items->push_back(item);
					return true;
				},
				ctx);
		}
	};

	Ctx ctx{&items, &includedSources};
	Ctx::enum_scene(scene, &ctx);
}

bool MotionCraftController::getSelectedScreenRect(int &x, int &y, int &w, int &h) const
{
	const auto screens = QGuiApplication::screens();
	for (auto *screen : screens) {
		if (!screen)
			continue;
		const QRect g = screen->geometry();
		const QString key = make_screen_key_from_rect(g.x(), g.y(), g.width(), g.height());
		if (screenKey.isEmpty() || key == screenKey) {
			x = g.x();
			y = g.y();
			w = g.width();
			h = g.height();
			return w > 0 && h > 0;
		}
	}
	return false;
}

bool MotionCraftController::getCursorPos(int &x, int &y) const
{
#if defined(__linux__)
	const QString platform = QGuiApplication::platformName();
	const bool nativeWayland = platform.startsWith(QStringLiteral("wayland"), Qt::CaseInsensitive);
	if (nativeWayland)
		return false;
#endif

	const QPoint p = QCursor::pos();
	x = p.x();
	y = p.y();
	return true;
}

static bool get_monitor_capture_selector(obs_source_t *src, QString &selector, int &monitorId, bool &hasId)
{
	selector.clear();
	monitorId = -1;
	hasId = false;

	if (!src)
		return false;

	obs_data_t *s = obs_source_get_settings(src);
	if (!s)
		return false;

	auto pick_str = [&](const char *key) -> const char * {
		const char *v = obs_data_get_string(s, key);
		return (v && *v) ? v : nullptr;
	};

	if (const char *v = pick_str("alt_id"))
		selector = QString::fromUtf8(v);
	else if (const char *v = pick_str("monitor_device"))
		selector = QString::fromUtf8(v);
	else if (const char *v = pick_str("display_device"))
		selector = QString::fromUtf8(v);
	else if (const char *v = pick_str("device"))
		selector = QString::fromUtf8(v);
	else if (const char *v = pick_str("monitor"))
		selector = QString::fromUtf8(v);
	else if (const char *v = pick_str("monitor_id"))
		selector = QString::fromUtf8(v);
	else if (const char *v = pick_str("id"))
		selector = QString::fromUtf8(v);
	else if (const char *v = pick_str("setting_id"))
		selector = QString::fromUtf8(v);

	if (selector.isEmpty()) {
		monitorId = (int)obs_data_get_int(s, "monitor_id");
		if (monitorId != 0)
			hasId = true;

		if (!hasId) {
			monitorId = (int)obs_data_get_int(s, "monitor");
			if (monitorId != 0)
				hasId = true;
		}
		if (!hasId) {
			monitorId = (int)obs_data_get_int(s, "display");
			if (monitorId != 0)
				hasId = true;
		}
		if (!hasId) {
			monitorId = (int)obs_data_get_int(s, "screen");
			if (monitorId != 0)
				hasId = true;
		}
	}

	obs_data_release(s);
	return (!selector.isEmpty()) || hasId;
}

#ifdef _WIN32
struct MonitorInfoLite {
	QString device;
	RECT rc{};
};

static std::vector<MonitorInfoLite> enum_monitors()
{
	std::vector<MonitorInfoLite> out;

	auto cb = [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
		auto *vec = reinterpret_cast<std::vector<MonitorInfoLite> *>(lp);
		MONITORINFOEXA mi{};
		mi.cbSize = sizeof(mi);
		if (!GetMonitorInfoA(hMon, &mi))
			return TRUE;
		MonitorInfoLite m;
		m.device = QString::fromLatin1(mi.szDevice);
		m.rc = mi.rcMonitor;
		vec->push_back(m);
		return TRUE;
	};

	EnumDisplayMonitors(nullptr, nullptr, cb, reinterpret_cast<LPARAM>(&out));
	return out;
}

#ifdef _WIN32
#include <vector>
#include <string>
#include <cwctype>
#include <windows.h>
#include <winuser.h>
#include <wingdi.h>

static bool wcs_ieq(const wchar_t *a, const wchar_t *b)
{
	if (!a || !b)
		return false;
	while (*a && *b) {
		if (towlower(*a) != towlower(*b))
			return false;
		++a;
		++b;
	}
	return *a == 0 && *b == 0;
}

static bool resolve_displayconfig_path_to_gdi(const QString &selector, QString &outGdi)
{
	outGdi.clear();
	if (selector.isEmpty())
		return false;

	const std::wstring want = selector.toStdWString();
	if (want.rfind(LR"(\\?\DISPLAY#)", 0) != 0)
		return false;

	UINT32 pathCount = 0, modeCount = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
		return false;

	std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
	    ERROR_SUCCESS)
		return false;

	for (UINT32 i = 0; i < pathCount; ++i) {
		const auto &p = paths[i];

		DISPLAYCONFIG_TARGET_DEVICE_NAME tdn{};
		tdn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		tdn.header.size = sizeof(tdn);
		tdn.header.adapterId = p.targetInfo.adapterId;
		tdn.header.id = p.targetInfo.id;

		if (DisplayConfigGetDeviceInfo(&tdn.header) != ERROR_SUCCESS)
			continue;

		if (!wcs_ieq(tdn.monitorDevicePath, want.c_str()))
			continue;

		DISPLAYCONFIG_SOURCE_DEVICE_NAME sdn{};
		sdn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		sdn.header.size = sizeof(sdn);
		sdn.header.adapterId = p.sourceInfo.adapterId;
		sdn.header.id = p.sourceInfo.id;

		if (DisplayConfigGetDeviceInfo(&sdn.header) != ERROR_SUCCESS)
			return false;

		outGdi = QString::fromWCharArray(sdn.viewGdiDeviceName);
		return !outGdi.isEmpty();
	}

	return false;
}
#endif

static bool match_monitor_rect(obs_source_t *src, RECT &rcOut)
{
	QString selector;
	int monId = -1;
	bool hasId = false;
	if (!get_monitor_capture_selector(src, selector, monId, hasId))
		return false;

#ifdef _WIN32
	if (!selector.isEmpty()) {
		QString gdi;
		if (resolve_displayconfig_path_to_gdi(selector, gdi) && !gdi.isEmpty()) {
			selector = gdi;
		}
	}
#endif

	auto mons = enum_monitors();
	if (mons.empty())
		return false;

	if (!selector.isEmpty()) {
		for (auto &m : mons) {
			if (m.device == selector) {
				rcOut = m.rc;
				return true;
			}
			QRegularExpression re("DISPLAY(\\d+)");
			auto mw = re.match(selector);
			auto md = re.match(m.device);
			if (mw.hasMatch() && md.hasMatch() && mw.captured(1) == md.captured(1)) {
				rcOut = m.rc;
				return true;
			}
		}
	}

	if (hasId) {
		int idx = monId;
		if (idx >= 1 && idx <= (int)mons.size() && !selector.isEmpty() == false) {
		}
		if (idx >= 0 && idx < (int)mons.size()) {
			rcOut = mons[(size_t)idx].rc;
			return true;
		}
		if (idx >= 1 && idx <= (int)mons.size()) {
			rcOut = mons[(size_t)(idx - 1)].rc;
			return true;
		}
	}

	return false;
}

static bool parse_obs_window_selector(const QString &sel, QString &title, QString &clazz, QString &exe)
{
	title.clear();
	clazz.clear();
	exe.clear();
	if (sel.isEmpty())
		return false;

	QString s = sel;
	int last = s.lastIndexOf(':');
	if (last <= 0) {
		title = s;
		return true;
	}
	exe = s.mid(last + 1);
	s = s.left(last);

	int mid = s.lastIndexOf(':');
	if (mid <= 0) {
		title = s;
		return true;
	}
	clazz = s.mid(mid + 1);
	title = s.left(mid);
	return true;
}

static bool mods_current(bool wantCtrl, bool wantAlt, bool wantShift, bool wantWin, bool wantLeftCtrl,
			 bool wantRightCtrl, bool wantLeftAlt, bool wantRightAlt, bool wantLeftShift,
			 bool wantRightShift, bool wantLeftWin, bool wantRightWin);

static bool mods_current(bool wantCtrl, bool wantAlt, bool wantShift, bool wantWin)
{
	return mods_current(wantCtrl, wantAlt, wantShift, wantWin, false, false, false, false, false, false, false,
			    false);
}

static std::wstring to_w(const QString &s)
{
	return s.toStdWString();
}

static bool get_process_exe_name(DWORD pid, QString &out)
{
	out.clear();
	HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
	if (!h)
		return false;
	wchar_t buf[MAX_PATH]{};
	if (GetModuleFileNameExW(h, nullptr, buf, MAX_PATH) == 0) {
		CloseHandle(h);
		return false;
	}
	CloseHandle(h);
	QString full = QString::fromWCharArray(buf);
	int slash = std::max(full.lastIndexOf('\\'), full.lastIndexOf('/'));
	out = (slash >= 0) ? full.mid(slash + 1) : full;
	return true;
}

struct WinFindCtx {
	QString wantTitle;
	QString wantClass;
	QString wantExe;
	HWND found = nullptr;
};

static BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lp)
{
	auto *ctx = reinterpret_cast<WinFindCtx *>(lp);
	if (!ctx)
		return TRUE;
	if (!IsWindowVisible(hwnd))
		return TRUE;

	wchar_t titleBuf[512]{};
	GetWindowTextW(hwnd, titleBuf, 512);
	QString title = QString::fromWCharArray(titleBuf);

	wchar_t classBuf[256]{};
	GetClassNameW(hwnd, classBuf, 256);
	QString clazz = QString::fromWCharArray(classBuf);

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	QString exe;
	if (pid)
		get_process_exe_name(pid, exe);

	auto norm = [](QString s) {
		s = s.trimmed();
		return s;
	};

	const QString wt = norm(ctx->wantTitle);
	const QString wc = norm(ctx->wantClass);
	const QString we = norm(ctx->wantExe);

	bool ok = true;
	if (!we.isEmpty())
		ok = ok && exe.compare(we, Qt::CaseInsensitive) == 0;
	if (!wc.isEmpty())
		ok = ok && clazz.compare(wc, Qt::CaseInsensitive) == 0;

	if (!wt.isEmpty()) {
		ok = ok && title.contains(wt, Qt::CaseInsensitive);
	}

	if (ok) {
		ctx->found = hwnd;
		return FALSE;
	}
	return TRUE;
}

static bool match_window_rect_for_source(obs_source_t *src, RECT &rcOut)
{
	obs_data_t *s = obs_source_get_settings(src);
	if (!s)
		return false;
	const char *w = obs_data_get_string(s, "window");
	QString sel = (w && *w) ? QString::fromUtf8(w) : QString();
	obs_data_release(s);

	if (sel.isEmpty())
		return false;

	QString title, clazz, exe;
	parse_obs_window_selector(sel, title, clazz, exe);

	WinFindCtx ctx;
	ctx.wantTitle = title;
	ctx.wantClass = clazz;
	ctx.wantExe = exe;

	EnumWindows(enum_windows_cb, reinterpret_cast<LPARAM>(&ctx));
	if (!ctx.found)
		return false;

	RECT rc{};
	if (!GetWindowRect(ctx.found, &rc))
		return false;

	rcOut = rc;
	return true;
}
#endif

#ifdef __APPLE__
struct MonitorInfoLite {
	CGDirectDisplayID displayID;
	CGRect rc;
};

static std::vector<MonitorInfoLite> enum_monitors()
{
	std::vector<MonitorInfoLite> out;
	uint32_t count = 0;
	CGGetActiveDisplayList(0, nullptr, &count);
	if (count == 0)
		return out;
	std::vector<CGDirectDisplayID> displays(count);
	CGGetActiveDisplayList(count, displays.data(), &count);
	for (uint32_t i = 0; i < count; i++) {
		MonitorInfoLite m;
		m.displayID = displays[i];
		m.rc = CGDisplayBounds(displays[i]);
		out.push_back(m);
	}
	return out;
}

static bool match_monitor_rect(obs_source_t *src, CGRect &rcOut)
{
	auto mons = enum_monitors();
	if (mons.empty())
		return false;

	obs_data_t *s = obs_source_get_settings(src);
	if (s) {
		const char *uuid = obs_data_get_string(s, "display_uuid");
		if (uuid && *uuid) {
			for (auto &m : mons) {
				CFUUIDRef duuid = CGDisplayCreateUUIDFromDisplayID(m.displayID);
				if (duuid) {
					CFStringRef uuidStr = CFUUIDCreateString(kCFAllocatorDefault, duuid);
					CFRelease(duuid);
					if (uuidStr) {
						char buf[128];
						if (CFStringGetCString(uuidStr, buf, sizeof(buf),
								       kCFStringEncodingUTF8)) {
							if (strcasecmp(buf, uuid) == 0) {
								CFRelease(uuidStr);
								obs_data_release(s);
								rcOut = m.rc;
								return true;
							}
						}
						CFRelease(uuidStr);
					}
				}
			}
		}

		if (obs_data_has_user_value(s, "display")) {
			int idx = (int)obs_data_get_int(s, "display");
			if (idx >= 0 && idx < (int)mons.size()) {
				obs_data_release(s);
				rcOut = mons[(size_t)idx].rc;
				return true;
			}
		}

		obs_data_release(s);
	}

	QString selector;
	int monId = -1;
	bool hasId = false;
	if (get_monitor_capture_selector(src, selector, monId, hasId) && hasId) {
		CGDirectDisplayID wantId = (CGDirectDisplayID)monId;
		for (auto &m : mons) {
			if (m.displayID == wantId) {
				rcOut = m.rc;
				return true;
			}
		}
	}

	if (mons.size() == 1) {
		rcOut = mons[0].rc;
		return true;
	}

	return false;
}

static bool match_window_rect_for_source(obs_source_t *src, CGRect &rcOut)
{
	obs_data_t *s = obs_source_get_settings(src);
	if (!s)
		return false;

	int64_t windowID = obs_data_get_int(s, "window");
	if (windowID == 0)
		windowID = obs_data_get_int(s, "window_id");
	const char *ownerName = obs_data_get_string(s, "owner_name");
	obs_data_release(s);

	CFArrayRef windowList = CGWindowListCopyWindowInfo(
		kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
	if (!windowList)
		return false;

	bool found = false;
	CFIndex count = CFArrayGetCount(windowList);
	for (CFIndex i = 0; i < count && !found; i++) {
		CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(windowList, i);

		if (windowID != 0) {
			CFNumberRef numRef = (CFNumberRef)CFDictionaryGetValue(dict, kCGWindowNumber);
			if (numRef) {
				int64_t wid = 0;
				CFNumberGetValue(numRef, kCFNumberSInt64Type, &wid);
				if (wid == windowID) {
					CFDictionaryRef boundsDict =
						(CFDictionaryRef)CFDictionaryGetValue(dict, kCGWindowBounds);
					if (boundsDict && CGRectMakeWithDictionaryRepresentation(boundsDict, &rcOut))
						found = true;
				}
			}
			continue;
		}

		if (ownerName && *ownerName) {
			CFStringRef owner = (CFStringRef)CFDictionaryGetValue(dict, kCGWindowOwnerName);
			if (owner) {
				char buf[256];
				if (CFStringGetCString(owner, buf, sizeof(buf), kCFStringEncodingUTF8)) {
					if (strcmp(buf, ownerName) == 0) {
						CFDictionaryRef boundsDict =
							(CFDictionaryRef)CFDictionaryGetValue(dict, kCGWindowBounds);
						if (boundsDict &&
						    CGRectMakeWithDictionaryRepresentation(boundsDict, &rcOut))
							found = true;
					}
				}
			}
		}
	}

	CFRelease(windowList);
	return found;
}
#endif

#ifdef __linux__
struct MonitorInfoLite {
	QString name;
	int x, y, w, h;
};

static std::vector<MonitorInfoLite> enum_monitors()
{
	std::vector<MonitorInfoLite> out;
	Display *dpy = XOpenDisplay(nullptr);
	if (!dpy)
		return out;

	int screen = DefaultScreen(dpy);
	Window root = RootWindow(dpy, screen);
	XRRScreenResources *res = XRRGetScreenResources(dpy, root);
	if (!res) {
		XCloseDisplay(dpy);
		return out;
	}

	for (int i = 0; i < res->noutput; i++) {
		XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
		if (!oi)
			continue;
		if (oi->connection != RR_Connected || oi->crtc == X11_None) {
			XRRFreeOutputInfo(oi);
			continue;
		}
		XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
		if (!ci) {
			XRRFreeOutputInfo(oi);
			continue;
		}
		MonitorInfoLite m;
		m.name = QString::fromLatin1(oi->name);
		m.x = (int)ci->x;
		m.y = (int)ci->y;
		m.w = (int)ci->width;
		m.h = (int)ci->height;
		out.push_back(m);
		XRRFreeCrtcInfo(ci);
		XRRFreeOutputInfo(oi);
	}

	XRRFreeScreenResources(res);
	XCloseDisplay(dpy);
	return out;
}

struct LinuxRect {
	int x, y, w, h;
};

static bool match_monitor_rect(obs_source_t *src, LinuxRect &rcOut)
{
	auto mons = enum_monitors();
	if (mons.empty())
		return false;

	QString selector;
	int monId = -1;
	bool hasId = false;
	get_monitor_capture_selector(src, selector, monId, hasId);

	if (!selector.isEmpty()) {
		for (auto &m : mons) {
			if (m.name == selector) {
				rcOut = {m.x, m.y, m.w, m.h};
				return true;
			}
		}
		QRegularExpression re("(\\d+)$");
		auto mw = re.match(selector);
		if (mw.hasMatch()) {
			int idx = mw.captured(1).toInt();
			if (idx >= 0 && idx < (int)mons.size()) {
				auto &m = mons[(size_t)idx];
				rcOut = {m.x, m.y, m.w, m.h};
				return true;
			}
		}
	}

	if (hasId) {
		if (monId >= 0 && monId < (int)mons.size()) {
			auto &m = mons[(size_t)monId];
			rcOut = {m.x, m.y, m.w, m.h};
			return true;
		}
		if (monId >= 1 && monId <= (int)mons.size()) {
			auto &m = mons[(size_t)(monId - 1)];
			rcOut = {m.x, m.y, m.w, m.h};
			return true;
		}
	}

	if (mons.size() == 1) {
		auto &m = mons[0];
		rcOut = {m.x, m.y, m.w, m.h};
		return true;
	}

	return false;
}

static bool parse_obs_window_selector_linux(const QString &sel, QString &title, QString &clazz, QString &exe)
{
	title.clear();
	clazz.clear();
	exe.clear();
	if (sel.isEmpty())
		return false;

	QString s = sel;
	int last = s.lastIndexOf(':');
	if (last <= 0) {
		title = s;
		return true;
	}
	exe = s.mid(last + 1);
	s = s.left(last);

	int mid = s.lastIndexOf(':');
	if (mid <= 0) {
		title = s;
		return true;
	}
	clazz = s.mid(mid + 1);
	title = s.left(mid);
	return true;
}

static bool match_window_rect_for_source(obs_source_t *src, LinuxRect &rcOut)
{
	obs_data_t *s = obs_source_get_settings(src);
	if (!s)
		return false;

	const char *w = obs_data_get_string(s, "window");
	QString sel = (w && *w) ? QString::fromUtf8(w) : QString();
	if (sel.isEmpty()) {
		w = obs_data_get_string(s, "capture_window");
		sel = (w && *w) ? QString::fromUtf8(w) : QString();
	}

	const char *windowName = obs_data_get_string(s, "window_name");
	QString wantName = (windowName && *windowName) ? QString::fromUtf8(windowName) : QString();

	int64_t windowId = obs_data_get_int(s, "window");
	obs_data_release(s);

	Display *dpy = XOpenDisplay(nullptr);
	if (!dpy)
		return false;

	if (windowId > 0) {
		XWindowAttributes attr;
		if (XGetWindowAttributes(dpy, (Window)windowId, &attr)) {
			int absX = 0, absY = 0;
			Window child;
			XTranslateCoordinates(dpy, (Window)windowId, DefaultRootWindow(dpy), 0, 0, &absX, &absY,
					      &child);
			XCloseDisplay(dpy);
			rcOut = {absX, absY, attr.width, attr.height};
			return true;
		}
	}

	QString title, clazz, exe;
	if (!sel.isEmpty())
		parse_obs_window_selector_linux(sel, title, clazz, exe);
	if (title.isEmpty() && !wantName.isEmpty())
		title = wantName;

	if (title.isEmpty() && clazz.isEmpty()) {
		XCloseDisplay(dpy);
		return false;
	}

	Window root = DefaultRootWindow(dpy);
	Atom netClientList = XInternAtom(dpy, "_NET_CLIENT_LIST", True);
	bool found = false;

	if (netClientList != X11_None) {
		Atom type;
		int format;
		unsigned long nitems, bytesAfter;
		unsigned char *data = nullptr;
		if (XGetWindowProperty(dpy, root, netClientList, 0, ~0L, False, XA_WINDOW, &type, &format, &nitems,
				       &bytesAfter, &data) == Success &&
		    data) {
			Window *windows = (Window *)data;
			for (unsigned long i = 0; i < nitems && !found; i++) {
				Window win = windows[i];

				if (!clazz.isEmpty()) {
					XClassHint ch{};
					if (XGetClassHint(dpy, win, &ch)) {
						QString resName = ch.res_name ? QString::fromUtf8(ch.res_name)
									      : QString();
						QString resClass = ch.res_class ? QString::fromUtf8(ch.res_class)
										: QString();
						if (ch.res_name)
							XFree(ch.res_name);
						if (ch.res_class)
							XFree(ch.res_class);
						if (resName.compare(clazz, Qt::CaseInsensitive) != 0 &&
						    resClass.compare(clazz, Qt::CaseInsensitive) != 0)
							continue;
					} else {
						continue;
					}
				}

				if (!title.isEmpty()) {
					char *name = nullptr;
					if (XFetchName(dpy, win, &name) && name) {
						QString winTitle = QString::fromUtf8(name);
						XFree(name);
						if (!winTitle.contains(title, Qt::CaseInsensitive))
							continue;
					} else {
						continue;
					}
				}

				XWindowAttributes attr;
				if (XGetWindowAttributes(dpy, win, &attr)) {
					int absX = 0, absY = 0;
					Window child;
					XTranslateCoordinates(dpy, win, root, 0, 0, &absX, &absY, &child);
					rcOut = {absX, absY, attr.width, attr.height};
					found = true;
				}
			}
			XFree(data);
		}
	}

	XCloseDisplay(dpy);
	return found;
}
#endif

bool MotionCraftController::mapCursorToScenePixels(int cursorX, int cursorY, float &sx, float &sy,
						  bool &cursorInside) const
{
	cursorInside = false;
	sx = 0.f;
	sy = 0.f;

	int rx = 0, ry = 0, rw = 0, rh = 0;
	if (!getSelectedScreenRect(rx, ry, rw, rh) || rw <= 0 || rh <= 0)
		return false;

	cursorInside = !(cursorX < rx || cursorX >= rx + rw || cursorY < ry || cursorY >= ry + rh);
	const int clampedX = std::max(rx, std::min(cursorX, rx + rw - 1));
	const int clampedY = std::max(ry, std::min(cursorY, ry + rh - 1));
	const double relX = (clampedX - rx) / (double)rw;
	const double relY = (clampedY - ry) / (double)rh;

	obs_video_info ovi{};
	const bool haveVi = obs_get_video_info(&ovi);
	const double cw = haveVi ? (double)ovi.base_width : 1920.0;
	const double ch = haveVi ? (double)ovi.base_height : 1080.0;
	if (cw <= 0.0 || ch <= 0.0)
		return false;

	if (centerCursorUntilEdge && sceneContentBoundsValid) {
		const double contentW = std::max(1.0, (double)sceneContentMax.x - (double)sceneContentMin.x);
		const double contentH = std::max(1.0, (double)sceneContentMax.y - (double)sceneContentMin.y);
		sx = (float)((double)sceneContentMin.x + relX * contentW);
		sy = (float)((double)sceneContentMin.y + relY * contentH);
	} else {
		sx = (float)(relX * cw);
		sy = (float)(relY * ch);
	}
	return true;
}

static void remove_legacy_marker_items(obs_scene_t *scene, obs_source_t *currentMarkerSource)
{
	if (!scene)
		return;

	struct Ctx {
		obs_source_t *current = nullptr;
		std::vector<obs_sceneitem_t *> items;

		static bool enum_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
		{
			auto *ctx = static_cast<Ctx *>(param);
			if (!ctx || !item)
				return true;

			obs_source_t *src = obs_sceneitem_get_source(item);
			if (!src || src == ctx->current)
				return true;

			const char *name = obs_source_get_name(src);
			const char *id = obs_source_get_id(src);
			const bool markerName = source_name_starts_with(name, kMotionCraftMarkerSourceName);
			const bool oldImageSource = id && QString::fromUtf8(id) == QStringLiteral("image_source");
			const bool oldProceduralMarker =
				id && QString::fromUtf8(id) == QString::fromUtf8(kMotionCraftMarkerSourceId) &&
				src != ctx->current;
			if (markerName && (oldImageSource || oldProceduralMarker))
				ctx->items.push_back(item);

			return true;
		}
	};

	Ctx ctx{currentMarkerSource, {}};
	obs_scene_enum_items(scene, Ctx::enum_cb, &ctx);
	for (obs_sceneitem_t *item : ctx.items)
		obs_sceneitem_remove(item);
}

static void cleanup_legacy_marker_items_all_scenes(obs_source_t *currentMarkerSource)
{
	obs_frontend_source_list scenes{};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *sceneSource = scenes.sources.array[i];
		obs_scene_t *scene = sceneSource ? obs_scene_from_source(sceneSource) : nullptr;
		if (scene)
			remove_legacy_marker_items(scene, currentMarkerSource);
	}
	obs_frontend_source_list_free(&scenes);
}

static void normalize_marker_scene_item(obs_sceneitem_t *item)
{
	if (!item)
		return;

	// The marker is a small source-local shader quad. Keep the scene item at
	// native size; otherwise OBS may reuse an old stretched/bounded transform and
	// the ring appears across the whole canvas instead of around the cursor.
	obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_NONE);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_rot(item, 0.0f);

	vec2 scale{};
	scale.x = 1.0f;
	scale.y = 1.0f;
	obs_sceneitem_set_scale(item, &scale);

	obs_sceneitem_crop crop{};
	obs_sceneitem_set_crop(item, &crop);

	obs_sceneitem_set_locked(item, true);
	obs_sceneitem_set_visible(item, true);
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP);
}

void MotionCraftController::rebuildMarkerImage()
{
	const int size = std::clamp(markerSize, 6, 512);
	const QString path = markerImagePath();
	if (path.isEmpty())
		return;

	ensure_parent_dir_exists(path);

	QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);

	QPainter painter(&img);
	painter.setRenderHint(QPainter::Antialiasing, true);

	QColor stroke = QColor::fromRgba(markerColor);
	if (!stroke.isValid())
		stroke = QColor(255, 0, 0, 255);
	if (stroke.alpha() == 0)
		stroke.setAlpha(255);
	stroke.setAlpha(255);

	const qreal thickness = std::clamp<qreal>((qreal)markerThickness, 1.0, std::max<qreal>(1.0, size / 2.0 - 2.0));
	const qreal margin = thickness * 0.5 + 1.5;
	const QRectF ringRect(margin, margin, size - margin * 2.0, size - margin * 2.0);

	QPen ringPen(stroke);
	ringPen.setWidthF(thickness);
	ringPen.setCosmetic(true);
	painter.setPen(ringPen);
	painter.setBrush(Qt::NoBrush);
	painter.drawEllipse(ringRect);
	painter.end();

	img.save(path, "PNG");
}

void MotionCraftController::ensureMarkerFilter()
{
	if (!markerSource)
		return;

	if (markerFilter) {
		obs_source_t *existing = obs_source_get_filter_by_name(markerSource, "MotionCraftOpacity");
		if (existing) {
			obs_source_release(existing);
			return;
		}
		obs_source_release(markerFilter);
		markerFilter = nullptr;
	}

	obs_data_t *fsettings = obs_data_create();
	obs_data_set_double(fsettings, "opacity", 100.0);
	markerFilter = obs_source_create_private("color_filter", "MotionCraftOpacity", fsettings);
	obs_data_release(fsettings);

	if (markerFilter)
		obs_source_filter_add(markerSource, markerFilter);
}

void MotionCraftController::applyMarkerOpacity(int opacity255)
{
	ensureMarkerFilter();
	if (!markerFilter)
		return;

	const int clamped = std::clamp(opacity255, 0, 255);
	if (markerCurrentOpacity == clamped)
		return;

	markerCurrentOpacity = clamped;
	const double opacityPct = clamped / 255.0 * 100.0;
	obs_data_t *fsettings = obs_data_create();
	obs_data_set_double(fsettings, "opacity", opacityPct);
	obs_source_update(markerFilter, fsettings);
	obs_data_release(fsettings);
}

void MotionCraftController::ensureMarkerSource()
{
	if (markerSource)
		return;

	rebuildMarkerImage();

	const QString path = markerImagePath();
	if (path.isEmpty())
		return;

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "file", path.toUtf8().constData());
	obs_data_set_bool(settings, "unload", false);
	markerSource = obs_source_create_private("image_source", kMotionCraftMarkerSourceName, settings);
	obs_data_release(settings);

	if (markerSource)
		ensureMarkerFilter();
}

obs_sceneitem_t *MotionCraftController::ensureMarkerItem(obs_scene_t *scene)
{
	if (!scene)
		return nullptr;

	ensureMarkerSource();
	if (!markerSource)
		return nullptr;

	remove_legacy_marker_items(scene, markerSource);

	struct Finder {
		obs_source_t *want = nullptr;
		obs_sceneitem_t *found = nullptr;

		static bool enum_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
		{
			auto *f = static_cast<Finder *>(param);
			if (!f || f->found)
				return false;
			obs_source_t *src = obs_sceneitem_get_source(item);
			if (src == f->want) {
				f->found = item;
				return false;
			}
			return true;
		}
	};

	Finder finder{markerSource, nullptr};
	obs_scene_enum_items(scene, Finder::enum_cb, &finder);
	if (finder.found) {
		normalize_marker_scene_item(finder.found);
		return finder.found;
	}

	obs_sceneitem_t *item = obs_scene_add(scene, markerSource);
	if (!item)
		return nullptr;

	normalize_marker_scene_item(item);
	return item;
}

void MotionCraftController::hideMarkerInScene(obs_scene_t *scene)
{
	if (!scene || !markerSource)
		return;

	remove_legacy_marker_items(scene, markerSource);

	struct Finder {
		obs_source_t *want = nullptr;
		obs_sceneitem_t *found = nullptr;

		static bool enum_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
		{
			auto *f = static_cast<Finder *>(param);
			if (!f || f->found)
				return false;
			obs_source_t *src = obs_sceneitem_get_source(item);
			if (src == f->want) {
				f->found = item;
				return false;
			}
			return true;
		}
	};

	Finder finder{markerSource, nullptr};
	obs_scene_enum_items(scene, Finder::enum_cb, &finder);
	if (finder.found)
		obs_sceneitem_set_visible(finder.found, false);
}

void MotionCraftController::updateMarkerAppearance()
{
	const uint32_t appearanceHash = ((uint32_t)std::clamp(markerSize, 6, 512) << 24) ^
					((uint32_t)std::clamp(markerThickness, 1, 64) << 16) ^ markerColor;

	if (markerSource && markerAppearanceHash == appearanceHash)
		return;

	rebuildMarkerImage();
	ensureMarkerSource();
	if (!markerSource)
		return;

	const QString path = markerImagePath();
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "file", path.toUtf8().constData());
	obs_data_set_bool(settings, "unload", false);
	obs_source_update(markerSource, settings);
	obs_data_release(settings);
	markerAppearanceHash = appearanceHash;
	markerCurrentOpacity = -1;
}

bool MotionCraftController::captureMarkerClickPosition()
{
	if (!showCursorMarker || !markerOnlyOnClick)
		return false;

	int cx = 0, cy = 0;
	float mx = 0.f, my = 0.f;
	bool inside = false;
	const bool mapped = getCursorPos(cx, cy) && mapCursorToScenePixels(cx, cy, mx, my, inside);
	if (!mapped || !inside)
		return false;

	markerClickX = mx;
	markerClickY = my;
	markerClickHasPos = true;
	const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
	static constexpr qint64 kMarkerFadeInMs = 110;
	static constexpr qint64 kMarkerHoldMs = 420;
	static constexpr qint64 kMarkerFadeOutMs = 220;
	markerClickFlashStartMs = nowMs;
	markerClickFlashHoldUntilMs = nowMs + kMarkerFadeInMs + kMarkerHoldMs;
	markerClickFlashFadeOutEndMs = markerClickFlashHoldUntilMs + kMarkerFadeOutMs;
	ensureTicking(true);
	return true;
}

bool MotionCraftController::isMarkerFlashActive(qint64 nowMs) const
{
	return showCursorMarker && markerOnlyOnClick && markerClickHasPos && markerClickFlashFadeOutEndMs > 0 &&
	       nowMs < markerClickFlashFadeOutEndMs;
}

int MotionCraftController::currentMarkerOpacity(qint64 nowMs)
{
	if (!showCursorMarker || !markerOnlyOnClick || !markerClickHasPos)
		return 0;

	static constexpr qint64 kMarkerFadeInMs = 110;
	if (markerClickFlashFadeOutEndMs <= 0 || nowMs >= markerClickFlashFadeOutEndMs) {
		markerClickHasPos = false;
		markerClickFlashStartMs = 0;
		markerClickFlashHoldUntilMs = 0;
		markerClickFlashFadeOutEndMs = 0;
		return 0;
	}

	if (nowMs < markerClickFlashStartMs + kMarkerFadeInMs) {
		const double tIn =
			clampd((double)(nowMs - markerClickFlashStartMs) / (double)std::max<qint64>(1, kMarkerFadeInMs),
			       0.0, 1.0);
		return (int)std::round(smoothstep(tIn) * 255.0);
	}

	if (nowMs < markerClickFlashHoldUntilMs)
		return 255;

	const qint64 fadeOutMs = std::max<qint64>(1, markerClickFlashFadeOutEndMs - markerClickFlashHoldUntilMs);
	const double tOut = clampd((double)(nowMs - markerClickFlashHoldUntilMs) / (double)fadeOutMs, 0.0, 1.0);
	return (int)std::round((1.0 - smoothstep(tOut)) * 255.0);
}

void MotionCraftController::updateMarkerPosition(obs_scene_t *scene, double x, double y, int opacity255)
{
	if (!showCursorMarker)
		return;

	const int clampedOpacity = std::clamp(opacity255, 0, 255);
	if (clampedOpacity <= 0) {
		hideMarkerInScene(scene);
		return;
	}

	obs_sceneitem_t *item = ensureMarkerItem(scene);
	if (!item)
		return;

	updateMarkerAppearance();
	applyMarkerOpacity(clampedOpacity);

	if (!scene_item_pointer_is_live(scene, item))
		return;

	normalize_marker_scene_item(item);

	vec2 pos{};
	pos.x = (float)x;
	pos.y = (float)y;
	obs_sceneitem_set_pos(item, &pos);

	if (!obs_sceneitem_visible(item)) {
		obs_sceneitem_set_visible(item, true);
		obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP);
	}
}

void MotionCraftController::captureOriginal(obs_sceneitem_t *item)
{
	if (!item)
		return;
	OrigState orig = readSceneItemTransform(item);
	if (!orig.valid)
		return;

	float renderedW = 0.0f;
	float renderedH = 0.0f;

	obs_source_t *src = obs_sceneitem_get_source(item);
	float visibleW = 0.0f;
	float visibleH = 0.0f;
	if (src) {
		const float sw = (float)obs_source_get_width(src);
		const float sh = (float)obs_source_get_height(src);
		visibleW = sw - (float)orig.crop.left - (float)orig.crop.right;
		visibleH = sh - (float)orig.crop.top - (float)orig.crop.bottom;
	}
	if (visibleW <= 0.0f)
		visibleW = 1.0f;
	if (visibleH <= 0.0f)
		visibleH = 1.0f;

	if (orig.boundsType == OBS_BOUNDS_NONE || orig.bounds.x <= 0.0f || orig.bounds.y <= 0.0f) {
		renderedW = visibleW * orig.scale.x;
		renderedH = visibleH * orig.scale.y;
	} else {
		const float sx = orig.bounds.x / visibleW;
		const float sy = orig.bounds.y / visibleH;

		renderedW = orig.bounds.x;
		renderedH = orig.bounds.y;
		orig.effectiveScale.x = sx;
		orig.effectiveScale.y = sy;
	}

	if (orig.boundsType == OBS_BOUNDS_NONE || orig.bounds.x <= 0.0f || orig.bounds.y <= 0.0f) {
		orig.effectiveScale.x = renderedW / visibleW;
		orig.effectiveScale.y = renderedH / visibleH;
	}

	/* libobs maps a scene item as
	 *     p_scene = R(rot) * (S * p_local - origin) + pos
	 * (update_item_transform, obs-scene.c), where origin is the alignment
	 * offset inside the scaled item. The zoom re-anchors every item to
	 * top-left alignment but deliberately leaves rot untouched, so the
	 * alignment origin must be rotated before it is subtracted. Subtracting it
	 * unrotated anchors a rotated source as if it were axis-aligned, which
	 * swings it off the canvas. Reduces to the old expression when rot == 0. */
	vec2 originOffset{};
	if (orig.align & OBS_ALIGN_RIGHT)
		originOffset.x = renderedW;
	else if (!(orig.align & OBS_ALIGN_LEFT))
		originOffset.x = renderedW * 0.5f;

	if (orig.align & OBS_ALIGN_BOTTOM)
		originOffset.y = renderedH;
	else if (!(orig.align & OBS_ALIGN_TOP))
		originOffset.y = renderedH * 0.5f;

	float originSin = 0.0f;
	float originCos = 1.0f;
	rotation_sin_cos(orig.rot, originSin, originCos);

	orig.effectivePos.x = orig.pos.x - (originOffset.x * originCos - originOffset.y * originSin);
	orig.effectivePos.y = orig.pos.y - (originOffset.x * originSin + originOffset.y * originCos);

	orig.valid = true;

	const QString key = sceneItemKey(item);
	if (!key.isEmpty())
		recoveryTransforms.insert(key, orig);

	SceneItemState state{};
	state.item = item;
	state.orig = orig;
	sceneItems.push_back(state);
}

void MotionCraftController::captureOriginalSceneItems(const std::vector<obs_sceneitem_t *> &items)
{
	sceneItems.clear();
	sceneContentBoundsValid = false;
	wiggleSafety = wiggleSafetyScale();
	for (auto *item : items)
		captureOriginal(item);

	if (recoveryActive && !sceneItems.empty())
		saveSettings();

	for (auto &state : sceneItems) {
		if (!state.item || !state.orig.valid)
			continue;
		/* Hidden items are still transformed (so they stay consistent if they
		 * are switched on mid-zoom) but must not drive the framing bounds:
		 * they show nothing, and scenes routinely park them far off-canvas. */
		if (!obs_sceneitem_visible(state.item))
			continue;
		obs_source_t *src = obs_sceneitem_get_source(state.item);
		if (!src)
			continue;
		const float sw = (float)obs_source_get_width(src);
		const float sh = (float)obs_source_get_height(src);
		const float visibleW = std::max(1.0f, sw - (float)state.orig.crop.left - (float)state.orig.crop.right);
		const float visibleH = std::max(1.0f, sh - (float)state.orig.crop.top - (float)state.orig.crop.bottom);
		const float renderedW = visibleW * state.orig.effectiveScale.x;
		const float renderedH = visibleH * state.orig.effectiveScale.y;

		/* Axis-aligned bounds of the item as it actually appears, i.e. of the
		 * rotated quad anchored at effectivePos. Treating a rotated item as an
		 * upright renderedW x renderedH box puts these bounds somewhere the
		 * source is not, and the framing clamp below then pushes the real
		 * source off-canvas by exactly that error. */
		float boundsSin = 0.0f;
		float boundsCos = 1.0f;
		rotation_sin_cos(state.orig.rot, boundsSin, boundsCos);

		const float cornerX[4] = {0.0f, renderedW, 0.0f, renderedW};
		const float cornerY[4] = {0.0f, 0.0f, renderedH, renderedH};

		float minX = state.orig.effectivePos.x;
		float minY = state.orig.effectivePos.y;
		float maxX = minX;
		float maxY = minY;
		for (int corner = 0; corner < 4; ++corner) {
			const float px =
				state.orig.effectivePos.x + cornerX[corner] * boundsCos - cornerY[corner] * boundsSin;
			const float py =
				state.orig.effectivePos.y + cornerX[corner] * boundsSin + cornerY[corner] * boundsCos;
			minX = std::min(minX, px);
			maxX = std::max(maxX, px);
			minY = std::min(minY, py);
			maxY = std::max(maxY, py);
		}

		state.framesContent = true;
		state.framingMin.x = minX;
		state.framingMin.y = minY;
		state.framingMax.x = maxX;
		state.framingMax.y = maxY;

		if (!sceneContentBoundsValid) {
			sceneContentMin.x = minX;
			sceneContentMin.y = minY;
			sceneContentMax.x = maxX;
			sceneContentMax.y = maxY;
			sceneContentBoundsValid = true;
		} else {
			sceneContentMin.x = std::min(sceneContentMin.x, minX);
			sceneContentMin.y = std::min(sceneContentMin.y, minY);
			sceneContentMax.x = std::max(sceneContentMax.x, maxX);
			sceneContentMax.y = std::max(sceneContentMax.y, maxY);
		}
	}
}

void MotionCraftController::restoreOriginal(obs_sceneitem_t *item)
{
	if (!item)
		return;
	for (const auto &state : sceneItems) {
		if (state.item != item || !state.orig.valid)
			continue;
		applySceneItemTransform(item, state.orig);
		return;
	}
}

void MotionCraftController::restoreOriginalSceneItems(const std::vector<obs_sceneitem_t *> &items)
{
	for (auto *item : items)
		restoreOriginal(item);
}

void MotionCraftController::restoreOriginalSceneItemsFromState()
{
	for (const auto &state : sceneItems) {
		if (state.item && state.orig.valid)
			applySceneItemTransform(state.item, state.orig);
	}
}

void MotionCraftController::applyZoomToScene(double z)
{
	if (sceneItems.empty())
		return;

	/* Scene and cursor come from the snapshot the Qt timer publishes; both
	 * obs_frontend_get_current_scene() and QCursor::pos() are main-thread only.
	 * Take our own ref under the lock so the main thread cannot release the
	 * scene out from under us mid-frame. */
	obs_source_t *sceneSource = nullptr;
	float snapX = 0.0f, snapY = 0.0f;
	bool snapMapped = false, snapInside = false;
	{
		std::lock_guard<std::mutex> lock(inputMutex);
		if (input.sceneRef)
			sceneSource = obs_source_get_ref(input.sceneRef);
		snapMapped = input.mapped;
		snapInside = input.inside;
		snapX = input.sceneX;
		snapY = input.sceneY;
	}

	obs_scene_t *scene = sceneSource ? obs_scene_from_source(sceneSource) : nullptr;
	if (!scene) {
		if (sceneSource)
			obs_source_release(sceneSource);
		return;
	}

	/* Released at every exit below; applyZoomToScene has several. */
	struct SceneRefGuard {
		obs_source_t *ref;
		~SceneRefGuard()
		{
			if (ref)
				obs_source_release(ref);
		}
	} sceneRefGuard{sceneSource};

	std::vector<obs_sceneitem_t *> liveItems;
	collect_live_scene_items(scene, liveItems);
	auto isLiveItem = [&liveItems](obs_sceneitem_t *item) {
		return item && std::find(liveItems.begin(), liveItems.end(), item) != liveItems.end();
	};

	obs_video_info ovi{};
	const bool haveVi = obs_get_video_info(&ovi);
	const double cw = haveVi ? (double)ovi.base_width : 1920.0;
	const double ch = haveVi ? (double)ovi.base_height : 1080.0;
	const double centerX = cw * 0.5;
	const double centerY = ch * 0.5;

	/* Wiggle is a camera move, not a per-item one: one drift is sampled here
	 * and every item is pushed, rolled and breathed by the same amount, about
	 * the same point the zoom uses. Moving items independently would pull a
	 * composed scene apart - the included-source list is what decides which
	 * layers the camera carries. */
	const bool wiggling = wiggleShaping();
	const double ampPos = wiggling ? clampd(wigglePositionPx, 0.0, kWigglePositionMaxPx) : 0.0;
	const double ampRot = wiggling ? clampd(wiggleRotationDeg, 0.0, kWiggleRotationMaxDeg) : 0.0;
	const double ampScale = wiggling ? clampd(wiggleScalePct, 0.0, kWiggleScaleMaxPct) / 100.0 : 0.0;

	double wiggleDriftX = 0.0;
	double wiggleDriftY = 0.0;
	double wiggleRoll = 0.0;
	double wiggleBreath = 1.0;
	if (wiggling) {
		const uint32_t seed = (uint32_t)wiggleSeed;
		wiggleDriftX = wiggle_noise(wigglePhase, seed) * ampPos;
		wiggleDriftY = wiggle_noise(wigglePhase, seed + 1000u) * ampPos;
		wiggleRoll = wiggle_noise(wigglePhase, seed + 2000u) * ampRot;
		wiggleBreath = 1.0 + wiggle_noise(wigglePhase, seed + 3000u) * ampScale;
	}

	/* Two zoom values from here on.
	 *
	 * zApply is what the items are actually drawn at: the requested zoom, times
	 * the safety enlargement the wiggle needs, times this frame's breath.
	 *
	 * z - which the framing clamp below is computed from - is the same thing at
	 * the smallest the breath will ever get. Clamping against the worst case is
	 * what makes the result valid for every frame: the applied box is this box
	 * scaled up about the canvas centre, and growing a box about a point inside
	 * the canvas cannot uncover an edge it already covered.
	 *
	 * With the wiggle off, safety is 1 and the breath is 1, so both collapse to
	 * the value that was passed in and nothing about the zoom changes. */
	const double safety = wiggling ? wiggleSafety : 1.0;
	const double zApply = z * safety * wiggleBreath;
	z *= safety * (1.0 - ampScale);

	float fx = (float)centerX;
	float fy = (float)centerY;
	float anchorX = (float)centerX;
	float anchorY = (float)centerY;
	float markerSceneX = fx;
	float markerSceneY = fy;
	bool markerHasPoint = false;

	const float mx = snapX;
	const float my = snapY;
	const bool mapped = snapMapped;
	(void)snapInside;

	const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
	if (mapped) {
		const double sampleDx = (double)mx - (double)lastCursorSampleX;
		const double sampleDy = (double)my - (double)lastCursorSampleY;
		const bool cursorMoved = !lastCursorSampleValid || sampleDx * sampleDx + sampleDy * sampleDy >= 0.25;
		if (cursorMoved) {
			lastCursorSampleX = mx;
			lastCursorSampleY = my;
			lastCursorSampleValid = true;
			lastCursorMovementMs = nowMs;
			mouseTrackingIdle = false;
		} else if (mouseIdleTimeoutMs > 0 && lastCursorMovementMs > 0 &&
			   nowMs - lastCursorMovementMs >= mouseIdleTimeoutMs) {
			mouseTrackingIdle = true;
		}
	}

	if (followMouse && followMouseRuntimeEnabled && !mouseTrackingIdle) {
		targetHasPos = false;
		if (mapped) {
			if (!followHasPos) {
				followX = mx;
				followY = my;
				followHasPos = true;
			} else {
				const double dx = (double)mx - (double)followX;
				const double dy = (double)my - (double)followY;
				const double dist = std::sqrt(dx * dx + dy * dy);

				const double effectiveSpeed = clampd(followSpeed, 0.25, 30.0);
				if (dist > 0.01) {
					const double response =
						1.0 - std::exp(-(2.0 + effectiveSpeed * 1.35) * tickDeltaSeconds);
					const double step = clampd(response, 0.01, 0.92);
					followX = (float)((double)followX + dx * step);
					followY = (float)((double)followY + dy * step);
				}
			}
			fx = followX;
			fy = followY;
			anchorX = (float)centerX;
			anchorY = (float)centerY;
		} else if (followHasPos) {
			fx = followX;
			fy = followY;
			anchorX = (float)centerX;
			anchorY = (float)centerY;
		}
	} else {
		if (!targetHasPos) {
			if (!followMouse) {
				/* Follow Mouse is off in settings, so the cursor should not
				 * influence the zoom at all - not even by seeding the focal
				 * point at trigger time. Zoom about the canvas centre.
				 * The runtime toggle is deliberately left alone below: that
				 * path freezes the zoom where the cursor last led it, which
				 * is the point of toggling mid-zoom. */
				targetX = (float)centerX;
				targetY = (float)centerY;
			} else if (followHasPos) {
				targetX = followX;
				targetY = followY;
			} else if (mapped) {
				targetX = mx;
				targetY = my;
			} else {
				targetX = (float)centerX;
				targetY = (float)centerY;
			}
			targetHasPos = true;
		}
		fx = targetX;
		fy = targetY;
		anchorX = (float)centerX;
		anchorY = (float)centerY;
	}

	if (showCursorMarker && markerClickHasPos) {
		markerSceneX = markerClickX;
		markerSceneY = markerClickY;
		markerHasPoint = true;
	}

	/* Framing clamp. The permitted pan is the INTERSECTION of the per-item
	 * intervals, not the interval of their union.
	 *
	 * The union is defined by the largest item, so it carries that item's slack
	 * against the canvas edge. Any targeted item smaller than the union can be
	 * panned right off the edge while the union still reports itself covered,
	 * and the canvas background shows through behind the smaller layers. A
	 * source whose bounds box overhangs the canvas is enough to trigger it: it
	 * frames itself and lets every other layer slide.
	 *
	 * Items too small to cover the canvas at this zoom are skipped rather than
	 * allowed to empty the intersection - a small overlay cannot frame anything
	 * and must not veto the items that can. If nothing can cover, or the items
	 * that can disagree, fall back to the union so the behaviour degrades to
	 * what it was rather than to something undefined. */
	double loX = 0.0, hiX = 0.0, loY = 0.0, hiY = 0.0;
	bool haveX = false, haveY = false;

	for (const auto &state : sceneItems) {
		if (!state.framesContent || !state.orig.valid || !isLiveItem(state.item))
			continue;

		const double itemMinX = (double)anchorX + ((double)state.framingMin.x - (double)fx) * z;
		const double itemMaxX = (double)anchorX + ((double)state.framingMax.x - (double)fx) * z;
		const double itemLoX = cw - itemMaxX;
		const double itemHiX = -itemMinX;
		if (itemLoX <= itemHiX) {
			loX = haveX ? std::max(loX, itemLoX) : itemLoX;
			hiX = haveX ? std::min(hiX, itemHiX) : itemHiX;
			haveX = true;
		}

		const double itemMinY = (double)anchorY + ((double)state.framingMin.y - (double)fy) * z;
		const double itemMaxY = (double)anchorY + ((double)state.framingMax.y - (double)fy) * z;
		const double itemLoY = ch - itemMaxY;
		const double itemHiY = -itemMinY;
		if (itemLoY <= itemHiY) {
			loY = haveY ? std::max(loY, itemLoY) : itemLoY;
			hiY = haveY ? std::min(hiY, itemHiY) : itemHiY;
			haveY = true;
		}
	}

	const double baseMinX = sceneContentBoundsValid ? (double)sceneContentMin.x : 0.0;
	const double baseMinY = sceneContentBoundsValid ? (double)sceneContentMin.y : 0.0;
	const double baseMaxX = sceneContentBoundsValid ? (double)sceneContentMax.x : cw;
	const double baseMaxY = sceneContentBoundsValid ? (double)sceneContentMax.y : ch;
	const double refMinX = (double)anchorX + (baseMinX - (double)fx) * z;
	const double refMaxX = (double)anchorX + (baseMaxX - (double)fx) * z;
	const double refMinY = (double)anchorY + (baseMinY - (double)fy) * z;
	const double refMaxY = (double)anchorY + (baseMaxY - (double)fy) * z;
	const double minOffsetX = cw - refMaxX;
	const double maxOffsetX = -refMinX;
	const double minOffsetY = ch - refMaxY;
	const double maxOffsetY = -refMinY;

	double offsetX = 0.0;
	double offsetY = 0.0;

	/* The interval the pan was actually clamped into. The wiggle's drift is a
	 * pan like any other, so it has to live inside the same interval - that is
	 * what keeps a drift at the far edge of a zoom from exposing the canvas
	 * background. Where there is slack the drift is unaffected; where the zoom
	 * has already been pushed flush against an edge it is what gives way. */
	double panLoX = 0.0, panHiX = 0.0;
	double panLoY = 0.0, panHiY = 0.0;

	/* Slack the roll needs kept back from the pan.
	 *
	 * Rolling the composition by th about the canvas centre carries a canvas
	 * point p to R(th)p, which is at most |p - centre| * 2sin(th/2) away, and
	 * no canvas point is further from the centre than half the diagonal. So the
	 * unrolled content covering the canvas grown by that much is enough for the
	 * rolled content to cover the canvas itself.
	 *
	 * Without this the framing clamp would spend the entire safety margin on
	 * panning closer to the zoom focus, leaving the content flush with the edge
	 * and the roll free to swing it off. Reserved from the maximum roll rather
	 * than this frame's, so the pan does not breathe with the wiggle. */
	const double rollMargin =
		wiggling ? std::hypot(cw, ch) * std::sin(ampRot * 3.14159265358979323846 / 360.0) : 0.0;
	auto reserveRoll = [rollMargin](double &lo, double &hi) {
		if (rollMargin <= 0.0)
			return;
		if (hi - lo <= 2.0 * rollMargin) {
			/* Not enough room to reserve: centring what there is spreads the
			 * shortfall evenly instead of dumping it all on one edge. */
			lo = hi = (lo + hi) * 0.5;
			return;
		}
		lo += rollMargin;
		hi -= rollMargin;
	};

	if (haveX && loX <= hiX) {
		panLoX = loX;
		panHiX = hiX;
	} else if (minOffsetX <= maxOffsetX) {
		panLoX = minOffsetX;
		panHiX = maxOffsetX;
	} else {
		panLoX = panHiX = (minOffsetX + maxOffsetX) * 0.5;
	}
	reserveRoll(panLoX, panHiX);
	offsetX = clampd(0.0, panLoX, panHiX);

	if (haveY && loY <= hiY) {
		panLoY = loY;
		panHiY = hiY;
	} else if (minOffsetY <= maxOffsetY) {
		panLoY = minOffsetY;
		panHiY = maxOffsetY;
	} else {
		panLoY = panHiY = (minOffsetY + maxOffsetY) * 0.5;
	}
	reserveRoll(panLoY, panHiY);
	offsetY = clampd(0.0, panLoY, panHiY);

	if (wiggling) {
		offsetX = clampd(offsetX + wiggleDriftX, panLoX, panHiX);
		offsetY = clampd(offsetY + wiggleDriftY, panLoY, panHiY);
	}

	const qint64 nowApplyMs = nowMs;
	/* Skipping a frame because the follow anchor has not moved is only free
	 * while nothing else is animating. A wiggle moves every frame by
	 * definition, so the rate limiter has to stand down or the drift stutters. */
	const bool steadyFollow = followMouse && followMouseRuntimeEnabled && !mouseTrackingIdle && !wiggling &&
				  !segmentRunning.load(std::memory_order_relaxed);
	if (steadyFollow) {
		const float dx = anchorX - lastFollowAnchorX;
		const float dy = anchorY - lastFollowAnchorY;
		const bool anchorMovedEnough = !lastFollowAnchorValid || ((dx * dx + dy * dy) >= 1.0f);
		if (!anchorMovedEnough && nowApplyMs - lastTransformApplyMs < 16) {
			std::lock_guard<std::mutex> lock(inputMutex);
			pendingMarkerVisible = showCursorMarker && markerHasPoint;
			pendingMarkerX = (double)anchorX + ((double)markerSceneX - (double)fx) * zApply + offsetX;
			pendingMarkerY = (double)anchorY + ((double)markerSceneY - (double)fy) * zApply + offsetY;
			return;
		}
	}

	lastTransformApplyMs = nowApplyMs;
	lastFollowAnchorX = anchorX;
	lastFollowAnchorY = anchorY;
	lastFollowAnchorValid = true;

	const uint32_t topLeftAlign = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	for (auto &state : sceneItems) {
		if (!state.item || !state.orig.valid || !isLiveItem(state.item))
			continue;

		if (!state.normalized) {
			if (obs_sceneitem_get_bounds_type(state.item) != OBS_BOUNDS_NONE)
				obs_sceneitem_set_bounds_type(state.item, OBS_BOUNDS_NONE);
			if (obs_sceneitem_get_alignment(state.item) != topLeftAlign)
				obs_sceneitem_set_alignment(state.item, topLeftAlign);
			state.normalized = true;
		}

		vec2 sc{};
		sc.x = state.orig.effectiveScale.x * (float)zApply;
		sc.y = state.orig.effectiveScale.y * (float)zApply;

		vec2 pos{};
		pos.x = (float)((double)anchorX + ((double)state.orig.effectivePos.x - (double)fx) * zApply + offsetX);
		pos.y = (float)((double)anchorY + ((double)state.orig.effectivePos.y - (double)fy) * zApply + offsetY);

		/* The roll turns the whole composition about the canvas centre, exactly
		 * as tilting a camera would, so an item's position rotates with it
		 * rather than the item spinning in place about its own anchor. The zoom
		 * re-anchors items to their top-left corner, which is precisely the
		 * pivot that would make spinning in place look wrong. */
		if (wiggleRoll != 0.0) {
			float rollSin = 0.0f, rollCos = 1.0f;
			rotation_sin_cos((float)wiggleRoll, rollSin, rollCos);
			const double dx = (double)pos.x - (double)anchorX;
			const double dy = (double)pos.y - (double)anchorY;
			pos.x = (float)((double)anchorX + dx * rollCos - dy * rollSin);
			pos.y = (float)((double)anchorY + dx * rollSin + dy * rollCos);
		}

		if (!state.lastAppliedValid || !nearly_equal_vec2(state.lastAppliedScale, sc, kScaleEpsilon)) {
			obs_sceneitem_set_scale(state.item, &sc);
			state.lastAppliedScale = sc;
		}

		if (!state.lastAppliedValid || !nearly_equal_vec2(state.lastAppliedPos, pos)) {
			obs_sceneitem_set_pos(state.item, &pos);
			state.lastAppliedPos = pos;
		}

		/* Written even when the wiggle is off, where the target is just the
		 * captured rotation: that is what puts the item back after the wiggle
		 * is switched off mid-zoom, and it is a no-op write in every other
		 * case because the value has not changed. */
		const float rotTarget = (float)((double)state.orig.rot + wiggleRoll);
		if (!state.lastAppliedValid || !nearly_equal(state.lastAppliedRot, rotTarget, 0.001f)) {
			obs_sceneitem_set_rot(state.item, rotTarget);
			state.lastAppliedRot = rotTarget;
		}

		state.lastAppliedValid = true;
	}

	/* Marker placement is published rather than applied: creating the marker
	 * source, rebuilding its image and setting filter values all mutate the
	 * scene graph, which must not happen from the graphics tick. The Qt timer
	 * picks this up. A cursor dot does not need frame-perfect timing; the zoom
	 * transform above does, which is the whole point of this split. */
	{
		std::lock_guard<std::mutex> lock(inputMutex);
		pendingMarkerVisible = showCursorMarker && markerHasPoint;
		pendingMarkerX = (double)anchorX + ((double)markerSceneX - (double)fx) * zApply + offsetX;
		pendingMarkerY = (double)anchorY + ((double)markerSceneY - (double)fy) * zApply + offsetY;
	}
}

/* Main-thread half of the loop. Samples everything the graphics thread is not
 * allowed to touch and publishes it. Deliberately does no animation work. */
void MotionCraftController::onTick()
{
	if (pendingFinish.exchange(false)) {
		finishZoomOnMainThread();
		return;
	}

	if (!tickingWanted.load(std::memory_order_acquire))
		return;

	if (!zoomActive.load(std::memory_order_acquire)) {
		std::vector<obs_sceneitem_t *> items;
		enumerateTargetItemsInCurrentScene(items);
		if (items.empty()) {
			if (debug)
				blog(LOG_WARNING, "[MotionCraft] No movable scene items in current scene.");
			tickingWanted.store(false, std::memory_order_release);
			ensureTicking(false);
			resetState();
			return;
		}

		/* Runs before zoomActive is published, so the graphics thread cannot be
		 * reading sceneItems while this mutates it. captureOriginalSceneItems
		 * may also saveSettings(), which does file IO - another reason it has
		 * to stay off the render path. */
		captureOriginalSceneItems(items);
		if (sceneItems.empty())
			return;
	}

	/* Refreshed here rather than only at capture so that changing an amplitude
	 * while the wiggle is running re-sizes the safety margin with it, instead
	 * of leaving the drift to overrun a margin bought for smaller numbers. */
	wiggleSafety = wiggleSafetyScale();

	obs_source_t *sceneSource = obs_frontend_get_current_scene();

	/* All marker scene-graph mutation happens here, on the main thread, using
	 * the placement the graphics tick published. */
	if (obs_scene_t *sc = sceneSource ? obs_scene_from_source(sceneSource) : nullptr) {
		bool markerVisible = false;
		double markerX = 0.0, markerY = 0.0;
		{
			std::lock_guard<std::mutex> lock(inputMutex);
			markerVisible = pendingMarkerVisible;
			markerX = pendingMarkerX;
			markerY = pendingMarkerY;
		}

		if (showCursorMarker && markerVisible) {
			const int opacity = currentMarkerOpacity(QDateTime::currentMSecsSinceEpoch());
			if (opacity > 0)
				updateMarkerPosition(sc, markerX, markerY, opacity);
			else
				hideMarkerInScene(sc);
		} else if (showCursorMarker) {
			hideMarkerInScene(sc);
		}
	}

	int cx = 0, cy = 0;
	float sx = 0.0f, sy = 0.0f;
	bool insideNow = false;
	const bool mappedNow = getCursorPos(cx, cy) && mapCursorToScenePixels(cx, cy, sx, sy, insideNow);

	obs_source_t *previousRef = nullptr;
	{
		std::lock_guard<std::mutex> lock(inputMutex);
		previousRef = input.sceneRef;
		input.sceneRef = sceneSource; /* ownership transferred */
		input.mapped = mappedNow;
		input.inside = insideNow;
		input.sceneX = sx;
		input.sceneY = sy;
	}
	if (previousRef)
		obs_source_release(previousRef);

	/* Published last: the graphics thread may start reading sceneItems the
	 * instant this lands, and by now it has a scene and a cursor sample. */
	zoomActive.store(true, std::memory_order_release);
}

/* Graphics-thread half. Called once per rendered frame with the real frame
 * delta, so motion is correct at any output frame rate. */
void MotionCraftController::videoTickCallback(void *param, float seconds)
{
	auto *self = static_cast<MotionCraftController *>(param);
	if (self)
		self->videoTick((double)seconds);
}

void MotionCraftController::videoTick(double seconds)
{
	if (!zoomActive.load(std::memory_order_acquire) || shuttingDown)
		return;

	/* Disabled mid-flight: stop where we are and hand back for teardown,
	 * rather than animating out. Same handshake as a zoom that finishes
	 * normally, so the restore still happens on the main thread. */
	if (abortRequested.exchange(false, std::memory_order_acq_rel)) {
		segmentRunning.store(false, std::memory_order_relaxed);
		currentZoom = 1.0;
		currentZoomVel = 0.0;
		zoomActive.store(false, std::memory_order_release);
		pendingFinish.store(true, std::memory_order_release);
		return;
	}

	/* OBS hands us the true frame delta. Clamp only to survive a stalled
	 * frame, not to impose a rate of our own. */
	tickDeltaSeconds = clampd(seconds, 1.0 / 480.0, 1.0 / 20.0);

	/* Retargeting happens here rather than in the key handler because the
	 * current zoom and velocity - the two things a seamless hand-off needs -
	 * only exist on this thread. */
	if (retargetRequested.exchange(false, std::memory_order_acq_rel))
		beginSegment(targetLevel.load(std::memory_order_relaxed));

	if (segmentRunning.load(std::memory_order_relaxed)) {
		segU += (segDurSec > 0.0) ? (tickDeltaSeconds / segDurSec) : 1.0;
		if (segU >= 1.0) {
			segU = 1.0;
			currentZoom = segZ1;
			currentZoomVel = 0.0;
			segmentRunning.store(false, std::memory_order_relaxed);
			if (segZ1 <= 1.0)
				targetHasPos = false;
		} else {
			hermite_ease(segU, segZ0, segZ1, segV0, segDurSec, currentZoom, currentZoomVel);
			/* Carried momentum can undershoot below the original size, which
			 * the framing clamp has no meaning for. Hold at 1.0; the curve
			 * is re-evaluated from segU next frame, so this never accumulates. */
			if (currentZoom < 1.0)
				currentZoom = 1.0;
		}
	}

	const bool wiggling = wiggleRunning.load(std::memory_order_acquire);

	if (!segmentRunning.load(std::memory_order_relaxed) && currentZoom <= 1.0 && !wiggling) {
		/* Stop touching shared state here, then let the main thread do the
		 * restore, the settings write and the reset. */
		zoomActive.store(false, std::memory_order_release);
		pendingFinish.store(true, std::memory_order_release);
		return;
	}

	if (wiggling)
		updateWiggleSpeed(tickDeltaSeconds);

	applyZoomToScene(currentZoom);
}

void MotionCraftController::finishZoomOnMainThread()
{
	restoringRecovery = true;
	restoreOriginalSceneItemsFromState();
	restoringRecovery = false;
	clearRecoveryActive();
	tickingWanted.store(false, std::memory_order_release);
	ensureTicking(false);
	resetState();

	obs_source_t *staleRef = nullptr;
	{
		std::lock_guard<std::mutex> lock(inputMutex);
		staleRef = input.sceneRef;
		input.sceneRef = nullptr;
		input.mapped = false;
	}
	if (staleRef)
		obs_source_release(staleRef);
}

static MotionCraftController *g_ctl = nullptr;

struct ModifierState {
	bool leftCtrl = false;
	bool rightCtrl = false;
	bool leftAlt = false;
	bool rightAlt = false;
	bool leftShift = false;
	bool rightShift = false;
	bool leftWin = false;
	bool rightWin = false;
};

static inline bool family_matches(bool wantAny, bool wantLeft, bool wantRight, bool haveLeft, bool haveRight)
{
	if (!wantAny && !wantLeft && !wantRight)
		return !haveLeft && !haveRight;
	if (wantLeft && !haveLeft)
		return false;
	if (wantRight && !haveRight)
		return false;
	if (!wantAny) {
		if (!wantLeft && haveLeft)
			return false;
		if (!wantRight && haveRight)
			return false;
	}
	return wantAny ? (haveLeft || haveRight) : true;
}

static ModifierState current_modifiers()
{
	ModifierState state;
#if defined(_WIN32)
	auto down = [](int vk) {
		return (GetAsyncKeyState(vk) & 0x8000) != 0;
	};
	state.leftCtrl = down(VK_LCONTROL);
	state.rightCtrl = down(VK_RCONTROL);
	state.leftAlt = down(VK_LMENU);
	state.rightAlt = down(VK_RMENU);
	state.leftShift = down(VK_LSHIFT);
	state.rightShift = down(VK_RSHIFT);
	state.leftWin = down(VK_LWIN);
	state.rightWin = down(VK_RWIN);
#elif defined(__APPLE__)
	auto down = [](int vk) {
		return CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, (CGKeyCode)vk);
	};
	state.leftCtrl = down(kVK_Control);
	state.rightCtrl = down(kVK_RightControl);
	state.leftAlt = down(kVK_Option);
	state.rightAlt = down(kVK_RightOption);
	state.leftShift = down(kVK_Shift);
	state.rightShift = down(kVK_RightShift);
	state.leftWin = down(kVK_Command);
	state.rightWin = down(kVK_RightCommand);
#elif defined(__linux__)
	Display *dpy = XOpenDisplay(nullptr);
	if (dpy) {
		char keys[32];
		XQueryKeymap(dpy, keys);
		auto isDown = [&](KeySym sym) -> bool {
			KeyCode kc = XKeysymToKeycode(dpy, sym);
			if (kc == 0)
				return false;
			return (keys[kc / 8] & (1 << (kc % 8))) != 0;
		};
		state.leftCtrl = isDown(XK_Control_L);
		state.rightCtrl = isDown(XK_Control_R);
		state.leftAlt = isDown(XK_Alt_L);
		state.rightAlt = isDown(XK_Alt_R);
		state.leftShift = isDown(XK_Shift_L);
		state.rightShift = isDown(XK_Shift_R);
		state.leftWin = isDown(XK_Super_L);
		state.rightWin = isDown(XK_Super_R);
		XCloseDisplay(dpy);
	}
#endif
	return state;
}

static bool mods_current(bool wantCtrl, bool wantAlt, bool wantShift, bool wantWin, bool wantLeftCtrl,
			 bool wantRightCtrl, bool wantLeftAlt, bool wantRightAlt, bool wantLeftShift,
			 bool wantRightShift, bool wantLeftWin, bool wantRightWin)
{
	const ModifierState state = current_modifiers();
	if (!family_matches(wantCtrl, wantLeftCtrl, wantRightCtrl, state.leftCtrl, state.rightCtrl))
		return false;
	if (!family_matches(wantAlt, wantLeftAlt, wantRightAlt, state.leftAlt, state.rightAlt))
		return false;
	if (!family_matches(wantShift, wantLeftShift, wantRightShift, state.leftShift, state.rightShift))
		return false;
	if (!family_matches(wantWin, wantLeftWin, wantRightWin, state.leftWin, state.rightWin))
		return false;
	return true;
}

#ifndef _WIN32
static bool mods_current(bool wantCtrl, bool wantAlt, bool wantShift, bool wantWin)
{
	return mods_current(wantCtrl, wantAlt, wantShift, wantWin, false, false, false, false, false, false, false,
			    false);
}
#endif

#ifdef _WIN32
static bool vk_matches(int pressedVk, int wantVk)
{
	if (pressedVk == wantVk)
		return true;

	if (wantVk >= '0' && wantVk <= '9') {
		const int d = wantVk - '0';
		return pressedVk == (VK_NUMPAD0 + d);
	}
	if (wantVk >= VK_NUMPAD0 && wantVk <= VK_NUMPAD9) {
		const int d = wantVk - VK_NUMPAD0;
		return pressedVk == ('0' + d);
	}

	return false;
}

LRESULT CALLBACK MotionCraftController::kb_hook_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode == HC_ACTION && g_ctl) {
		const auto *k = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
		const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
		const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

		if (!k || !(down || up))
			return CallNextHookEx((HHOOK)g_ctl->keyboardHook, nCode, wParam, lParam);

		const int vk = (int)k->vkCode;

		const auto &master = g_ctl->pluginToggle;
		if (down && master.valid && vk_matches(vk, master.vk) &&
		    mods_current(master.modCtrl, master.modAlt, master.modShift, master.modWin)) {
			g_ctl->togglePluginEnabled();
			return CallNextHookEx((HHOOK)g_ctl->keyboardHook, nCode, wParam, lParam);
		}

		if (!g_ctl->pluginEnabled)
			return CallNextHookEx((HHOOK)g_ctl->keyboardHook, nCode, wParam, lParam);

		if (g_ctl->hotkeysEnabled && g_ctl->followToggleHkValid && down && g_ctl->followToggleHotkeyVk != 0 &&
		    vk_matches(vk, g_ctl->followToggleHotkeyVk) &&
		    mods_current(g_ctl->followToggleModCtrl, g_ctl->followToggleModAlt, g_ctl->followToggleModShift,
				 g_ctl->followToggleModWin)) {
			g_ctl->toggleFollowMouseRuntime();
		}

		if (down) {
			for (int i = 0; i < MotionCraftController::kZoomLevelCount; i++) {
				const auto &lv = g_ctl->zoomLevels[i];
				if (!lv.valid || lv.vk == 0 || !vk_matches(vk, lv.vk))
					continue;
				if (!mods_current(lv.modCtrl, lv.modAlt, lv.modShift, lv.modWin))
					continue;
				g_ctl->activateLevel(i + 1);
				break;
			}
		}
	}
	return CallNextHookEx((HHOOK)g_ctl->keyboardHook, nCode, wParam, lParam);
}

LRESULT CALLBACK MotionCraftController::mouse_hook_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode == HC_ACTION && g_ctl) {
		const auto *m = reinterpret_cast<MSLLHOOKSTRUCT *>(lParam);
		if (!m)
			return CallNextHookEx((HHOOK)g_ctl->mouseHook, nCode, wParam, lParam);

		const bool down = (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN ||
				   wParam == WM_XBUTTONDOWN);

		if (down)
			g_ctl->captureMarkerClickPosition();
	}
	return CallNextHookEx((HHOOK)g_ctl->mouseHook, nCode, wParam, lParam);
}
#endif

#ifdef __APPLE__
static bool vk_matches(int pressedVk, int wantVk)
{
	if (pressedVk == wantVk)
		return true;

	if (wantVk >= kVK_ANSI_Keypad0 && wantVk <= kVK_ANSI_Keypad9) {
		static const int numRow[] = {kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
					     kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9};
		return pressedVk == numRow[wantVk - kVK_ANSI_Keypad0];
	}
	return false;
}

CGEventRef MotionCraftController::eventTapCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event,
						  void *refcon)
{
	(void)proxy;
	auto *ctl = static_cast<MotionCraftController *>(refcon);
	if (!ctl)
		return event;

	if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
		if (ctl->eventTap)
			CGEventTapEnable(ctl->eventTap, true);
		return event;
	}

	if (type == kCGEventKeyDown || type == kCGEventKeyUp) {
		const int keycode = (int)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
		const bool down = (type == kCGEventKeyDown);

		const auto &master = ctl->pluginToggle;
		const bool masterPressed = down && master.valid && vk_matches(keycode, master.vk) &&
					   mods_current(master.modCtrl, master.modAlt, master.modShift, master.modWin);
		if (masterPressed)
			ctl->togglePluginEnabled();

		/* The macOS event tap is shared with the cursor-halo mouse path, so it
		 * stays installed even with hotkeys off; the gate has to be here. */
		if (!masterPressed && ctl->pluginEnabled && ctl->hotkeysEnabled && ctl->followToggleHkValid && down &&
		    ctl->followToggleHotkeyVk != 0 &&
		    vk_matches(keycode, ctl->followToggleHotkeyVk) &&
		    mods_current(ctl->followToggleModCtrl, ctl->followToggleModAlt, ctl->followToggleModShift,
				 ctl->followToggleModWin)) {
			ctl->toggleFollowMouseRuntime();
		}

		if (down && !masterPressed && ctl->pluginEnabled) {
			for (int i = 0; i < MotionCraftController::kZoomLevelCount; i++) {
				const auto &lv = ctl->zoomLevels[i];
				if (!lv.valid || lv.vk == 0 || !vk_matches(keycode, lv.vk))
					continue;
				if (!mods_current(lv.modCtrl, lv.modAlt, lv.modShift, lv.modWin))
					continue;
				ctl->activateLevel(i + 1);
				break;
			}
		}
	}

	const bool isMouseDown =
		(type == kCGEventLeftMouseDown || type == kCGEventRightMouseDown || type == kCGEventOtherMouseDown);

	if (isMouseDown)
		ctl->captureMarkerClickPosition();

	return event;
}
#endif

#ifdef __linux__
static bool vk_matches(int pressedVk, int wantVk)
{
	if (pressedVk == wantVk)
		return true;

	if (wantVk >= XK_a && wantVk <= XK_z)
		return pressedVk == (wantVk - XK_a + XK_A);
	if (wantVk >= XK_A && wantVk <= XK_Z)
		return pressedVk == (wantVk - XK_A + XK_a);

	if (wantVk >= XK_KP_0 && wantVk <= XK_KP_9)
		return pressedVk == (XK_0 + (wantVk - XK_KP_0));
	if (wantVk >= XK_0 && wantVk <= XK_9)
		return pressedVk == (XK_KP_0 + (wantVk - XK_0));

	return false;
}

void MotionCraftController::processXInput2Events()
{
	if (!xiDisplay)
		return;

	while (XPending(xiDisplay)) {
		XEvent ev;
		XNextEvent(xiDisplay, &ev);

		if (ev.xcookie.type != GenericEvent || ev.xcookie.extension != xiOpcode)
			continue;
		if (!XGetEventData(xiDisplay, &ev.xcookie))
			continue;

		const int evtype = ev.xcookie.evtype;

		if (evtype == XI_RawKeyPress || evtype == XI_RawKeyRelease) {
			XIRawEvent *raw = (XIRawEvent *)ev.xcookie.data;
			const int keycode = raw->detail;
			KeySym sym = XkbKeycodeToKeysym(xiDisplay, keycode, 0, 0);
			const bool down = (evtype == XI_RawKeyPress);

			const bool masterPressed =
				down && pluginToggle.valid && vk_matches((int)sym, pluginToggle.vk) &&
				mods_current(pluginToggle.modCtrl, pluginToggle.modAlt, pluginToggle.modShift,
					     pluginToggle.modWin);
			if (masterPressed)
				togglePluginEnabled();

			/* Same as macOS: one XInput2 connection serves both the keys and
			 * the cursor halo, so it can outlive the hotkeys being disabled. */
			if (!masterPressed && pluginEnabled && hotkeysEnabled && followToggleHkValid && down &&
			    followToggleHotkeyVk != 0 && vk_matches((int)sym, followToggleHotkeyVk) &&
			    mods_current(followToggleModCtrl, followToggleModAlt, followToggleModShift,
					 followToggleModWin)) {
				toggleFollowMouseRuntime();
			}

			if (down && !masterPressed && pluginEnabled) {
				for (int i = 0; i < kZoomLevelCount; i++) {
					const ZoomLevel &lv = zoomLevels[i];
					if (!lv.valid || lv.vk == 0 || !vk_matches((int)sym, lv.vk))
						continue;
					if (!mods_current(lv.modCtrl, lv.modAlt, lv.modShift, lv.modWin))
						continue;
					activateLevel(i + 1);
					break;
				}
			}
		} else if (evtype == XI_RawButtonPress) {
			XIRawEvent *raw = (XIRawEvent *)ev.xcookie.data;
			const int button = raw->detail;

			if (button < 4 || button > 7)
				captureMarkerClickPosition();
		}

		XFreeEventData(xiDisplay, &ev.xcookie);
	}
}
#endif

void MotionCraftController::toggleFollowMouseRuntime()
{
	/* Only ever reached from a hotkey, so it obeys the same focus policy. */
	if (!pluginEnabled || !hotkeysEnabled || !hotkeyFocusAllows())
		return;

	followMouseRuntimeEnabled = !followMouseRuntimeEnabled;
	if (!followMouseRuntimeEnabled && followHasPos) {
		targetX = followX;
		targetY = followY;
		targetHasPos = true;
	} else {
		targetHasPos = false;
	}
	followHasPos = false;

	if (debug) {
		blog(LOG_INFO, "[MotionCraft] Follow mouse %s", followMouseRuntimeEnabled ? "ENABLED" : "DISABLED");
	}
}

static int qtKeyToVk(int qtKey)
{
#if defined(__APPLE__)
	if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
		static const int map[] = {kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F,
					  kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L,
					  kVK_ANSI_M, kVK_ANSI_N, kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R,
					  kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U, kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X,
					  kVK_ANSI_Y, kVK_ANSI_Z};
		return map[qtKey - Qt::Key_A];
	}
	if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) {
		static const int map[] = {kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
					  kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9};
		return map[qtKey - Qt::Key_0];
	}
	if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F20) {
		static const int map[] = {kVK_F1,  kVK_F2,  kVK_F3,  kVK_F4,  kVK_F5,  kVK_F6,  kVK_F7,
					  kVK_F8,  kVK_F9,  kVK_F10, kVK_F11, kVK_F12, kVK_F13, kVK_F14,
					  kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20};
		return map[qtKey - Qt::Key_F1];
	}
	switch (qtKey) {
	case Qt::Key_Space:
		return kVK_Space;
	case Qt::Key_Return:
	case Qt::Key_Enter:
		return kVK_Return;
	case Qt::Key_Escape:
		return kVK_Escape;
	case Qt::Key_Tab:
		return kVK_Tab;
	case Qt::Key_Backspace:
		return kVK_Delete;
	case Qt::Key_Delete:
		return kVK_ForwardDelete;
	case Qt::Key_Left:
		return kVK_LeftArrow;
	case Qt::Key_Right:
		return kVK_RightArrow;
	case Qt::Key_Up:
		return kVK_UpArrow;
	case Qt::Key_Down:
		return kVK_DownArrow;
	case Qt::Key_Home:
		return kVK_Home;
	case Qt::Key_End:
		return kVK_End;
	case Qt::Key_PageUp:
		return kVK_PageUp;
	case Qt::Key_PageDown:
		return kVK_PageDown;
	case Qt::Key_CapsLock:
		return kVK_CapsLock;
	case Qt::Key_Shift:
		return kVK_Shift;
	case Qt::Key_Control:
		return kVK_Control;
	case Qt::Key_Alt:
		return kVK_Option;
	case Qt::Key_Meta:
		return kVK_Command;
	case Qt::Key_Semicolon:
		return kVK_ANSI_Semicolon;
	case Qt::Key_Equal:
		return kVK_ANSI_Equal;
	case Qt::Key_Comma:
		return kVK_ANSI_Comma;
	case Qt::Key_Minus:
		return kVK_ANSI_Minus;
	case Qt::Key_Period:
		return kVK_ANSI_Period;
	case Qt::Key_Slash:
		return kVK_ANSI_Slash;
	case Qt::Key_QuoteLeft:
		return kVK_ANSI_Grave;
	case Qt::Key_BracketLeft:
		return kVK_ANSI_LeftBracket;
	case Qt::Key_Backslash:
		return kVK_ANSI_Backslash;
	case Qt::Key_BracketRight:
		return kVK_ANSI_RightBracket;
	default:
		break;
	}
	return 0;
#elif defined(__linux__)
	if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z)
		return XK_a + (qtKey - Qt::Key_A);
	if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9)
		return XK_0 + (qtKey - Qt::Key_0);
	if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24)
		return XK_F1 + (qtKey - Qt::Key_F1);

	switch (qtKey) {
	case Qt::Key_Space:
		return XK_space;
	case Qt::Key_Return:
	case Qt::Key_Enter:
		return XK_Return;
	case Qt::Key_Escape:
		return XK_Escape;
	case Qt::Key_Tab:
		return XK_Tab;
	case Qt::Key_Backspace:
		return XK_BackSpace;
	case Qt::Key_Delete:
		return XK_Delete;
	case Qt::Key_Left:
		return XK_Left;
	case Qt::Key_Right:
		return XK_Right;
	case Qt::Key_Up:
		return XK_Up;
	case Qt::Key_Down:
		return XK_Down;
	case Qt::Key_Home:
		return XK_Home;
	case Qt::Key_End:
		return XK_End;
	case Qt::Key_PageUp:
		return XK_Page_Up;
	case Qt::Key_PageDown:
		return XK_Page_Down;
	case Qt::Key_Insert:
		return XK_Insert;
	case Qt::Key_Print:
		return XK_Print;
	case Qt::Key_Pause:
		return XK_Pause;
	case Qt::Key_CapsLock:
		return XK_Caps_Lock;
	case Qt::Key_Shift:
		return XK_Shift_L;
	case Qt::Key_Control:
		return XK_Control_L;
	case Qt::Key_Alt:
		return XK_Alt_L;
	case Qt::Key_Meta:
		return XK_Super_L;
	case Qt::Key_Semicolon:
		return XK_semicolon;
	case Qt::Key_Equal:
		return XK_equal;
	case Qt::Key_Comma:
		return XK_comma;
	case Qt::Key_Minus:
		return XK_minus;
	case Qt::Key_Period:
		return XK_period;
	case Qt::Key_Slash:
		return XK_slash;
	case Qt::Key_QuoteLeft:
		return XK_grave;
	case Qt::Key_BracketLeft:
		return XK_bracketleft;
	case Qt::Key_Backslash:
		return XK_backslash;
	case Qt::Key_BracketRight:
		return XK_bracketright;
	default:
		break;
	}

	if (qtKey >= 0x20 && qtKey <= 0x7E)
		return qtKey;

	return 0;
#else
	if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z)
		return 'A' + (qtKey - Qt::Key_A);
	if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9)
		return '0' + (qtKey - Qt::Key_0);
	if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24)
		return VK_F1 + (qtKey - Qt::Key_F1);

	switch (qtKey) {
	case Qt::Key_Space:
		return VK_SPACE;
	case Qt::Key_Return:
	case Qt::Key_Enter:
		return VK_RETURN;
	case Qt::Key_Escape:
		return VK_ESCAPE;
	case Qt::Key_Tab:
		return VK_TAB;
	case Qt::Key_Backspace:
		return VK_BACK;
	case Qt::Key_Left:
		return VK_LEFT;
	case Qt::Key_Right:
		return VK_RIGHT;
	case Qt::Key_Up:
		return VK_UP;
	case Qt::Key_Down:
		return VK_DOWN;
	case Qt::Key_Insert:
		return VK_INSERT;
	case Qt::Key_Delete:
		return VK_DELETE;
	case Qt::Key_Home:
		return VK_HOME;
	case Qt::Key_End:
		return VK_END;
	case Qt::Key_PageUp:
		return VK_PRIOR;
	case Qt::Key_PageDown:
		return VK_NEXT;
	case Qt::Key_Print:
		return VK_SNAPSHOT;
	case Qt::Key_Pause:
		return VK_PAUSE;
	case Qt::Key_CapsLock:
		return VK_CAPITAL;
	case Qt::Key_Clear:
		return VK_CLEAR;
	case Qt::Key_Shift:
		return VK_SHIFT;
	case Qt::Key_Control:
		return VK_CONTROL;
	case Qt::Key_Alt:
		return VK_MENU;
	case Qt::Key_Meta:
		return VK_LWIN;
	case Qt::Key_Semicolon:
		return VK_OEM_1;
	case Qt::Key_Plus:
		return VK_OEM_PLUS;
	case Qt::Key_Comma:
		return VK_OEM_COMMA;
	case Qt::Key_Minus:
		return VK_OEM_MINUS;
	case Qt::Key_Period:
		return VK_OEM_PERIOD;
	case Qt::Key_Slash:
		return VK_OEM_2;
	case Qt::Key_QuoteLeft:
		return VK_OEM_3;
	case Qt::Key_BracketLeft:
		return VK_OEM_4;
	case Qt::Key_Backslash:
		return VK_OEM_5;
	case Qt::Key_BracketRight:
		return VK_OEM_6;

	default:
		break;
	}

	if (qtKey >= 0x20 && qtKey <= 0x7E)
		return qtKey;

	return 0;
#endif
}

/* Level keys accept an optional modifier combination but must carry a real key:
 * a bare Shift or Ctrl is far too easy to fire by accident when five of them
 * are bound at once, so it is rejected rather than treated as a trigger. */
/* Resolve a stored key sequence into what the hooks compare against. A bare
 * modifier is rejected: a trigger that fires on Ctrl alone would go off
 * constantly. The Follow Mouse toggle deliberately allows one and parses its
 * own sequence below, which is the only reason that code is not this code. */
static void parse_key_trigger(const QString &sequence, MotionCraftController::KeyTrigger &out)
{
	out = MotionCraftController::KeyTrigger{};

	QKeySequence seq(sequence);
	if (seq.isEmpty())
		return;

	const QKeyCombination kc = seq[0];
	const int key = int(kc.key());
	switch (key) {
	case Qt::Key_unknown:
	case Qt::Key_Control:
	case Qt::Key_Shift:
	case Qt::Key_Alt:
	case Qt::Key_Meta:
		return;
	default:
		break;
	}

	const auto mods = kc.keyboardModifiers();
	out.vk = qtKeyToVk(key);
	out.modCtrl = mods.testFlag(Qt::ControlModifier);
	out.modAlt = mods.testFlag(Qt::AltModifier);
	out.modShift = mods.testFlag(Qt::ShiftModifier);
	out.modWin = mods.testFlag(Qt::MetaModifier);
	out.valid = (out.vk != 0);
}

static void parse_level_hotkey(MotionCraftController::ZoomLevel &lv)
{
	MotionCraftController::KeyTrigger t;
	parse_key_trigger(lv.hotkey, t);
	lv.vk = t.vk;
	lv.valid = t.valid;
	lv.modCtrl = t.modCtrl;
	lv.modAlt = t.modAlt;
	lv.modShift = t.modShift;
	lv.modWin = t.modWin;
}

void MotionCraftController::rebuildTriggersFromSettings()
{
	for (ZoomLevel &lv : zoomLevels)
		parse_level_hotkey(lv);

	parse_key_trigger(pluginToggleHotkeySequence, pluginToggle);

	followToggleHkValid = false;
	followToggleHotkeyVk = 0;
	followToggleModCtrl = false;
	followToggleModAlt = false;
	followToggleModShift = false;
	followToggleModWin = false;

	QKeySequence followSeq(followToggleHotkeySequence);
	if (!followSeq.isEmpty()) {
		const QKeyCombination kc = followSeq[0];
		const auto mods = kc.keyboardModifiers();
		const int key = int(kc.key());
		followToggleHotkeyVk = qtKeyToVk(key);
		followToggleModCtrl = mods.testFlag(Qt::ControlModifier);
		followToggleModAlt = mods.testFlag(Qt::AltModifier);
		followToggleModShift = mods.testFlag(Qt::ShiftModifier);
		followToggleModWin = mods.testFlag(Qt::MetaModifier);

#ifdef _WIN32
		const bool keyIsModifier = (followToggleHotkeyVk == VK_CONTROL || followToggleHotkeyVk == VK_LCONTROL ||
					    followToggleHotkeyVk == VK_RCONTROL || followToggleHotkeyVk == VK_MENU ||
					    followToggleHotkeyVk == VK_LMENU || followToggleHotkeyVk == VK_RMENU ||
					    followToggleHotkeyVk == VK_SHIFT || followToggleHotkeyVk == VK_LSHIFT ||
					    followToggleHotkeyVk == VK_RSHIFT || followToggleHotkeyVk == VK_LWIN ||
					    followToggleHotkeyVk == VK_RWIN);
#elif defined(__APPLE__)
		const bool keyIsModifier =
			(followToggleHotkeyVk == kVK_Control || followToggleHotkeyVk == kVK_RightControl ||
			 followToggleHotkeyVk == kVK_Option || followToggleHotkeyVk == kVK_RightOption ||
			 followToggleHotkeyVk == kVK_Shift || followToggleHotkeyVk == kVK_RightShift ||
			 followToggleHotkeyVk == kVK_Command || followToggleHotkeyVk == kVK_RightCommand);
#elif defined(__linux__)
		const bool keyIsModifier = (followToggleHotkeyVk == XK_Control_L ||
					    followToggleHotkeyVk == XK_Control_R || followToggleHotkeyVk == XK_Alt_L ||
					    followToggleHotkeyVk == XK_Alt_R || followToggleHotkeyVk == XK_Shift_L ||
					    followToggleHotkeyVk == XK_Shift_R || followToggleHotkeyVk == XK_Super_L ||
					    followToggleHotkeyVk == XK_Super_R);
#else
		const bool keyIsModifier = false;
#endif

		if (keyIsModifier && mods == Qt::NoModifier) {
#ifdef _WIN32
			followToggleModCtrl = (followToggleHotkeyVk == VK_CONTROL ||
					       followToggleHotkeyVk == VK_LCONTROL ||
					       followToggleHotkeyVk == VK_RCONTROL);
			followToggleModAlt = (followToggleHotkeyVk == VK_MENU || followToggleHotkeyVk == VK_LMENU ||
					      followToggleHotkeyVk == VK_RMENU);
			followToggleModShift = (followToggleHotkeyVk == VK_SHIFT || followToggleHotkeyVk == VK_LSHIFT ||
						followToggleHotkeyVk == VK_RSHIFT);
			followToggleModWin = (followToggleHotkeyVk == VK_LWIN || followToggleHotkeyVk == VK_RWIN);
#elif defined(__APPLE__)
			followToggleModCtrl =
				(followToggleHotkeyVk == kVK_Control || followToggleHotkeyVk == kVK_RightControl);
			followToggleModAlt =
				(followToggleHotkeyVk == kVK_Option || followToggleHotkeyVk == kVK_RightOption);
			followToggleModShift =
				(followToggleHotkeyVk == kVK_Shift || followToggleHotkeyVk == kVK_RightShift);
			followToggleModWin =
				(followToggleHotkeyVk == kVK_Command || followToggleHotkeyVk == kVK_RightCommand);
#elif defined(__linux__)
			followToggleModCtrl =
				(followToggleHotkeyVk == XK_Control_L || followToggleHotkeyVk == XK_Control_R);
			followToggleModAlt = (followToggleHotkeyVk == XK_Alt_L || followToggleHotkeyVk == XK_Alt_R);
			followToggleModShift =
				(followToggleHotkeyVk == XK_Shift_L || followToggleHotkeyVk == XK_Shift_R);
			followToggleModWin = (followToggleHotkeyVk == XK_Super_L || followToggleHotkeyVk == XK_Super_R);
#else
			followToggleHotkeyVk = 0;
#endif
		}

		followToggleHkValid = (followToggleHotkeyVk != 0) &&
				      (followToggleModCtrl || followToggleModAlt || followToggleModShift ||
				       followToggleModWin || key != 0);
	}
}

/* OBS's Settings -> Advanced -> Hotkeys policy, applied to our own hooks.
 *
 * OBS enforces this by calling obs_hotkey_enable_background_press() whenever
 * the application activation state changes. This plugin does not register
 * obs_hotkey_* entries - it installs low-level OS hooks so triggers work while
 * other applications have focus - so that call never reaches it and the setting
 * was simply ignored. Mirrors OBSApp::UpdateHotkeyFocusSetting and
 * ResetHotkeyState, including their definition of "in focus" as the whole
 * application being active rather than one specific window, so OBS dialogs and
 * floating docks count as focused just as they do for built-in hotkeys.
 *
 * Read fresh rather than cached so a change in OBS's settings applies at once.
 * Called only once a hotkey has already matched, so ordinary typing never
 * reaches it. */
bool MotionCraftController::hotkeyFocusAllows() const
{
	config_t *cfg = obs_frontend_get_user_config();
	if (!cfg)
		return true;

	const char *focusType = config_get_string(cfg, "General", "HotkeyFocusType");
	if (!focusType || !*focusType)
		return true;

	bool enableInFocus = true;
	bool enableOutOfFocus = true;

	const QString policy = QString::fromUtf8(focusType);
	if (policy.compare(QStringLiteral("DisableHotkeysInFocus"), Qt::CaseInsensitive) == 0)
		enableInFocus = false;
	else if (policy.compare(QStringLiteral("DisableHotkeysOutOfFocus"), Qt::CaseInsensitive) == 0)
		enableOutOfFocus = false;

	const bool inFocus = (QGuiApplication::applicationState() == Qt::ApplicationActive);
	return inFocus ? enableInFocus : enableOutOfFocus;
}

bool MotionCraftController::anyLevelHotkeyValid() const
{
	for (const ZoomLevel &lv : zoomLevels) {
		if (lv.valid)
			return true;
	}
	return false;
}

bool MotionCraftController::needsKeyboardHook() const
{
	/* The toggle key has to stay heard even with the plugin off - it is the
	 * only way back on. Everything else is silenced. */
	if (pluginToggle.valid)
		return true;
	return pluginEnabled && hotkeysEnabled && (anyLevelHotkeyValid() || followToggleHkValid);
}

bool MotionCraftController::needsMouseHook() const
{
	/* The mouse hook only exists for the cursor halo now that no trigger can
	 * be bound to a mouse button. */
	return pluginEnabled && showCursorMarker;
}

void MotionCraftController::installHooks()
{
	if (!needsKeyboardHook() && !needsMouseHook())
		return;

#ifdef _WIN32
	g_ctl = this;

	if (needsKeyboardHook() && !keyboardHook) {
		keyboardHook = (void *)SetWindowsHookExW(WH_KEYBOARD_LL, kb_hook_proc, GetModuleHandleW(nullptr), 0);
	}
	if (needsMouseHook() && !mouseHook) {
		mouseHook = (void *)SetWindowsHookExW(WH_MOUSE_LL, mouse_hook_proc, GetModuleHandleW(nullptr), 0);
	}
#elif defined(__APPLE__)
	g_ctl = this;

	if (!eventTap) {
		CGEventMask mask = 0;
		if (needsKeyboardHook()) {
			mask |= CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) |
				CGEventMaskBit(kCGEventFlagsChanged);
		}
		if (needsMouseHook()) {
			mask |= CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp) |
				CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp) |
				CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp);
		}
		eventTap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly,
					    mask, eventTapCallback, this);
		if (eventTap) {
			runLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, eventTap, 0);
			CFRunLoopAddSource(CFRunLoopGetMain(), runLoopSource, kCFRunLoopCommonModes);
			CGEventTapEnable(eventTap, true);
		} else {
			blog(LOG_WARNING,
			     "[MotionCraft] Failed to create CGEventTap. "
			     "Grant Accessibility permission to OBS in System Settings > Privacy & Security > Accessibility.");
		}
	}
#elif defined(__linux__)
	g_ctl = this;

	const QString platform = QGuiApplication::platformName();
	if (platform.startsWith(QStringLiteral("wayland"), Qt::CaseInsensitive)) {
		blog(LOG_WARNING,
		     "[MotionCraft] Native Wayland session detected. XInput2 hooks and global cursor tracking are disabled. "
		     "The Global Shortcuts portal can provide hotkeys, but Wayland has no passive global cursor-position "
		     "portal; full tracking requires a compositor-supported input-capture workflow.");
		return;
	}

	if (!xiDisplay) {
		xiDisplay = XOpenDisplay(nullptr);
		if (!xiDisplay) {
			blog(LOG_WARNING, "[MotionCraft] Failed to open X11 display for input hooks.");
			return;
		}

		int event, error;
		if (!XQueryExtension(xiDisplay, "XInputExtension", &xiOpcode, &event, &error)) {
			blog(LOG_WARNING, "[MotionCraft] XInput2 extension not available.");
			XCloseDisplay(xiDisplay);
			xiDisplay = nullptr;
			return;
		}

		int major = 2, minor = 0;
		if (XIQueryVersion(xiDisplay, &major, &minor) != Success) {
			blog(LOG_WARNING, "[MotionCraft] XInput2 version query failed.");
			XCloseDisplay(xiDisplay);
			xiDisplay = nullptr;
			return;
		}

		unsigned char mask_bits[(XI_LASTEVENT + 7) / 8] = {};
		XIEventMask evmask;
		evmask.deviceid = XIAllMasterDevices;
		evmask.mask_len = sizeof(mask_bits);
		evmask.mask = mask_bits;
		if (needsKeyboardHook()) {
			XISetMask(mask_bits, XI_RawKeyPress);
			XISetMask(mask_bits, XI_RawKeyRelease);
		}
		if (needsMouseHook()) {
			XISetMask(mask_bits, XI_RawButtonPress);
			XISetMask(mask_bits, XI_RawButtonRelease);
		}
		XISelectEvents(xiDisplay, DefaultRootWindow(xiDisplay), &evmask, 1);
		XFlush(xiDisplay);

		int fd = ConnectionNumber(xiDisplay);
		xiNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
		connect(xiNotifier, &QSocketNotifier::activated, this, &MotionCraftController::processXInput2Events);

		blog(LOG_INFO, "[MotionCraft] XInput2 global input hooks installed (XI %d.%d).", major, minor);
	}
#endif
}

void MotionCraftController::uninstallHooks()
{
#ifdef _WIN32
	if (keyboardHook) {
		UnhookWindowsHookEx((HHOOK)keyboardHook);
		keyboardHook = nullptr;
	}
	if (mouseHook) {
		UnhookWindowsHookEx((HHOOK)mouseHook);
		mouseHook = nullptr;
	}
	g_ctl = nullptr;
#elif defined(__APPLE__)
	if (runLoopSource) {
		CFRunLoopRemoveSource(CFRunLoopGetMain(), runLoopSource, kCFRunLoopCommonModes);
		CFRelease(runLoopSource);
		runLoopSource = nullptr;
	}
	if (eventTap) {
		CGEventTapEnable(eventTap, false);
		CFRelease(eventTap);
		eventTap = nullptr;
	}
	g_ctl = nullptr;
#elif defined(__linux__)
	if (xiNotifier) {
		delete xiNotifier;
		xiNotifier = nullptr;
	}
	if (xiDisplay) {
		XCloseDisplay(xiDisplay);
		xiDisplay = nullptr;
	}
	g_ctl = nullptr;
#endif
}
