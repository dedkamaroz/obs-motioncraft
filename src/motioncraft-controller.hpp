#pragma once

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QObject>
#include <QPointer>
#include <QKeySequence>
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>
#include <QElapsedTimer>
#include <QString>
#include <QHash>
#include <atomic>
#include <mutex>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef WIN32_LEAN_AND_MEAN
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#elif defined(__linux__)

struct _XDisplay;
#endif

class MotionCraftDialog;

class MotionCraftController final : public QObject {
	Q_OBJECT

public:
	static MotionCraftController &instance();

	void initialize();
	void shutdown();
	void showDialog();
	void saveSettings();
	void loadSettings();
	void notifySettingsChanged();
	void rebuildRuntimeHooks();

	QString screenKey;
	QString followToggleHotkeySequence;
	QString pluginToggleHotkeySequence;

	/* A key plus its modifiers, resolved to whatever this platform's hooks
	 * compare against. Derived from the stored sequence by
	 * rebuildTriggersFromSettings(); never persisted in this form. */
	struct KeyTrigger {
		int vk = 0;
		bool valid = false;
		bool modCtrl = false;
		bool modAlt = false;
		bool modShift = false;
		bool modWin = false;
	};

	/* One assignable zoom level. `inMs` / `outMs` are not segment durations:
	 * they are time coordinates on a timeline whose origin is the unzoomed
	 * state, so inMs is "how long a zoom in from 1.0 to this level takes".
	 * The duration of any other transition is the distance between the two
	 * endpoints on that timeline - see timelineMsForZoom(). */
	struct ZoomLevel {
		double zoom = 1.0;
		int inMs = 0;
		int outMs = 0;
		QString hotkey;

		/* Derived by rebuildTriggersFromSettings(); not persisted. */
		int vk = 0;
		bool valid = false;
		bool modCtrl = false;
		bool modAlt = false;
		bool modShift = false;
		bool modWin = false;
	};

	static constexpr int kZoomLevelCount = 5;
	ZoomLevel zoomLevels[kZoomLevelCount];

	/* Master switch for every hotkey the plugin listens for - the level keys
	 * and the Follow Mouse toggle. Off means the keyboard hook is not
	 * installed at all, so nothing is intercepted system-wide. */
	bool hotkeysEnabled = true;

	/* The plugin's own on/off. Off unwinds any zoom and wiggle, puts every
	 * transform back and stops listening for everything except the key that
	 * turns it on again - so it costs nothing while it is off.
	 *
	 * Runtime state, not a setting: OBS always starts enabled, the same way
	 * the Follow Mouse runtime toggle does. A plugin that silently stayed off
	 * across a restart would just look broken. The key binding IS persisted. */
	bool pluginEnabled = true;
	KeyTrigger pluginToggle;

	void setPluginEnabled(bool on);
	void togglePluginEnabled();

	bool followMouse = true;
	bool followMouseRuntimeEnabled = true;
	double followSpeed = 8.0;
	bool centerCursorUntilEdge = true;
	int mouseIdleTimeoutMs = 0;
	bool portraitCover = true;
	bool showCursorMarker = false;
	bool markerOnlyOnClick = true;
	uint32_t markerColor = 0xFFFF0000;
	int markerSize = 26;
	int markerThickness = 4;
	bool debug = false;

	/* Wiggle: a handheld-camera drift laid on top of whatever the zoom is
	 * doing, driven by the same captured transforms. The ranges are
	 * deliberately narrow - this is meant to read as a camera operator
	 * breathing, not as a shake effect, and anything larger has to be paid
	 * for with a bigger safety scale (see wiggleSafetyScale). */
	static constexpr double kWigglePositionMaxPx = 5.0;
	static constexpr double kWiggleRotationMaxDeg = 1.0;
	static constexpr double kWiggleScaleMaxPct = 2.0;
	static constexpr double kWiggleSpeedMax = 5.0;

	/* How often a new speed is drawn from [min, max], and the time constant of
	 * the glide onto it. The glide is short enough that the speed is settled
	 * for most of each window but never steps discontinuously. */
	static constexpr double kWiggleSpeedSampleSec = 0.5;
	static constexpr double kWiggleSpeedGlideSec = 0.08;

	bool wiggleEnabled = false;
	double wigglePositionPx = 2.0;
	double wiggleRotationDeg = 0.3;
	double wiggleScalePct = 0.5;
	double wiggleSpeedMin = 1.0;
	double wiggleSpeedMax = 2.0;
	int wiggleSeed = 1234;

	void setWiggleEnabled(bool on);

	QSet<QString> includedSources;

signals:
	void settingsChanged();

private slots:
	void onTick();

private:
	MotionCraftController();
	~MotionCraftController() override;
	MotionCraftController(const MotionCraftController &) = delete;
	MotionCraftController &operator=(const MotionCraftController &) = delete;

	QString configPath() const;
	QString legacyConfigPath() const;

	void ensureTicking(bool on);
	void resetState();
	void rebuildTriggersFromSettings();
	void installHooks();
	void uninstallHooks();
	bool needsKeyboardHook() const;
	bool needsMouseHook() const;
	bool anyLevelHotkeyValid() const;
	bool hotkeyFocusAllows() const;
	void toggleFollowMouseRuntime();

	/* Level 0 is the unzoomed state; 1..kZoomLevelCount index zoomLevels[].
	 * activateLevel is the hotkey path and toggles: pressing the key of the
	 * level already requested returns to 0. requestLevel is the absolute form. */
	void activateLevel(int level);
	void requestLevel(int next);
	double levelZoom(int level) const;
	double timelineMsForZoom(double z, bool zoomingIn) const;
	void beginSegment(int level);

	/* True only when wiggle is running AND some amplitude is actually set;
	 * an all-zero wiggle must cost nothing and must not force the safety
	 * scale on. */
	bool wiggleShaping() const;
	double wiggleSafetyScale() const;
	void updateWiggleSpeed(double seconds);

	bool getSelectedScreenRect(int &x, int &y, int &w, int &h) const;
	void enumerateTargetItemsInCurrentScene(std::vector<obs_sceneitem_t *> &items) const;

	bool getCursorPos(int &x, int &y) const;
	bool mapCursorToScenePixels(int cursorX, int cursorY, float &sx, float &sy, bool &cursorInside) const;

	void captureOriginal(obs_sceneitem_t *item);
	void restoreOriginal(obs_sceneitem_t *item);
	void captureOriginalSceneItems(const std::vector<obs_sceneitem_t *> &items);
	void restoreOriginalSceneItems(const std::vector<obs_sceneitem_t *> &items);
	void restoreOriginalSceneItemsFromState();
	QString markerImagePath() const;
	void ensureMarkerSource();
	obs_sceneitem_t *ensureMarkerItem(obs_scene_t *scene);
	void hideMarkerInScene(obs_scene_t *scene);
	void rebuildMarkerImage();
	void ensureMarkerFilter();
	void applyMarkerOpacity(int opacity255);
	void updateMarkerAppearance();
	void updateMarkerPosition(obs_scene_t *scene, double x, double y, int opacity255);
	bool captureMarkerClickPosition();
	bool isMarkerFlashActive(qint64 nowMs) const;
	int currentMarkerOpacity(qint64 nowMs);
	void applyZoomToScene(double z);

	/* Split loop. Frame-critical work runs on OBS's graphics thread via
	 * obs_add_tick_callback so the transform lands exactly once per rendered
	 * frame, at whatever output fps is configured - a free-running Qt timer
	 * beats against the render clock and drops or doubles updates.
	 *
	 * Qt GUI calls (QCursor::pos, QGuiApplication::screens) and the whole
	 * obs_frontend_* API are main-thread only, and saveSettings() does blocking
	 * file IO, so all of that stays on the Qt timer, which publishes an
	 * InputSnapshot for the graphics thread to consume. */
	static void videoTickCallback(void *param, float seconds);
	void videoTick(double seconds);
	void finishZoomOnMainThread();

	struct InputSnapshot {
		bool mapped = false;
		bool inside = false;
		float sceneX = 0.0f;
		float sceneY = 0.0f;
		obs_source_t *sceneRef = nullptr; /* strong ref owned by the snapshot */
	};

	std::mutex inputMutex; /* guards `input` and the pendingMarker* placement */
	InputSnapshot input;
	bool pendingMarkerVisible = false;
	double pendingMarkerX = 0.0;
	double pendingMarkerY = 0.0;
	bool tickCallbackAdded = false;
	std::atomic<bool> tickingWanted{false};
	std::atomic<bool> pendingFinish{false};

	QTimer tickTimer;
	/* Release-stored by the main thread once capture completes, acquire-loaded
	 * by the graphics thread; that ordering is what makes sceneItems safe to
	 * read there without holding a lock across the transform writes. */
	std::atomic<bool> zoomActive{false};

	/* Last level the user asked for. Main-thread only: it is the toggle state,
	 * not the animation state, so it must not be disturbed by the tick. */
	int requestedLevel = 0;

	/* A transition is one cubic Hermite segment from (segZ0, segV0) to
	 * (segZ1, 0) over segDurSec. Carrying the entry velocity is what keeps an
	 * interrupted transition from changing speed abruptly; with segV0 == 0 the
	 * curve reduces exactly to segZ0 + (segZ1 - segZ0) * smoothstep(u), which
	 * is the ramp every uninterrupted zoom uses. */
	double segZ0 = 1.0;
	double segZ1 = 1.0;
	double segV0 = 0.0;
	double segDurSec = 0.0;
	double segU = 1.0;
	double currentZoom = 1.0;
	double currentZoomVel = 0.0;

	/* Published by the main thread (hook callbacks), consumed by the graphics
	 * thread, which is the only place the current zoom and velocity are known
	 * and therefore the only place a new segment can be seeded. */
	std::atomic<int> targetLevel{0};
	std::atomic<bool> retargetRequested{false};
	std::atomic<bool> segmentRunning{false};

	/* Set by the main thread when the user turns wiggle on or off, read by the
	 * graphics thread. It keeps the tick alive at zoom 1.0, which is the only
	 * structural difference between wiggling and zooming. */
	std::atomic<bool> wiggleRunning{false};

	/* Asks the graphics thread to drop everything and hand back to the main
	 * thread for teardown. Routed through the same pendingFinish handshake as
	 * a zoom that ends on its own, because that handshake is the only thing
	 * that guarantees the graphics thread has stopped reading sceneItems
	 * before the main thread clears it. */
	std::atomic<bool> abortRequested{false};

	/* Graphics-thread only. Phase is integrated rather than derived from a
	 * clock times speed, so a speed change bends the motion instead of
	 * teleporting it. */
	double wigglePhase = 0.0;
	double wiggleSpeedCurrent = 0.0;
	double wiggleSpeedTarget = 0.0;
	double wiggleSpeedTimer = 0.0;
	uint32_t wiggleSpeedRng = 0;

	/* Computed once per capture: how much every item has to be enlarged so the
	 * drift cannot pull the canvas background into view. */
	double wiggleSafety = 1.0;

	bool followHasPos = false;
	float followX = 0.0f;
	float followY = 0.0f;
	bool lastCursorSampleValid = false;
	float lastCursorSampleX = 0.0f;
	float lastCursorSampleY = 0.0f;
	qint64 lastCursorMovementMs = 0;
	bool mouseTrackingIdle = false;

	bool targetHasPos = false;
	double tickDeltaSeconds = 1.0 / 60.0;
	qint64 lastTickMs = 0;
	qint64 lastTransformApplyMs = 0;
	float lastFollowAnchorX = 0.0f;
	float lastFollowAnchorY = 0.0f;
	bool lastFollowAnchorValid = false;
	float targetX = 0.0f;
	float targetY = 0.0f;

	bool markerClickHasPos = false;
	float markerClickX = 0.0f;
	float markerClickY = 0.0f;
	uint32_t markerAppearanceHash = 0;
	int markerCurrentOpacity = -1;
	obs_source_t *markerFilter = nullptr;
	qint64 markerClickFlashStartMs = 0;
	qint64 markerClickFlashHoldUntilMs = 0;
	qint64 markerClickFlashFadeOutEndMs = 0;

	struct OrigState {
		bool valid = false;
		vec2 pos{};
		vec2 scale{};
		float rot = 0.0f;
		uint32_t align = 0;
		obs_bounds_type boundsType = OBS_BOUNDS_NONE;
		uint32_t boundsAlign = 0;
		vec2 bounds{};
		obs_sceneitem_crop crop{};
		vec2 effectivePos{};
		vec2 effectiveScale{};
	};

	struct SceneItemState {
		obs_sceneitem_t *item = nullptr;
		OrigState orig;
		bool normalized = false;
		bool lastAppliedValid = false;
		vec2 lastAppliedPos{};
		vec2 lastAppliedScale{};
		float lastAppliedRot = 0.0f;

		/* Unzoomed on-screen bounds of this item alone, used by the framing
		 * clamp. False for items that were hidden when the zoom started: they
		 * show nothing, so they must not constrain the framing. */
		bool framesContent = false;
		vec2 framingMin{};
		vec2 framingMax{};
	};

	QString sceneItemKey(obs_sceneitem_t *item) const;
	OrigState readSceneItemTransform(obs_sceneitem_t *item) const;
	void applySceneItemTransform(obs_sceneitem_t *item, const OrigState &state);
	void loadRecoveryMap(obs_data_t *data);
	void saveRecoveryMap(obs_data_t *data);
	void scheduleSettingsSave(int delayMs = 250);
	void restoreRecoveryIfNeeded();
	void markRecoveryActive();
	void clearRecoveryActive();
	void requestRecoveryRestore();
	static void frontendEventCallback(enum obs_frontend_event event, void *data);

	std::vector<SceneItemState> sceneItems;
	QHash<QString, OrigState> recoveryTransforms;
	bool pendingSettingsSave = false;
	bool shuttingDown = false;
	bool recoveryActive = false;
	bool restoringRecovery = false;
	bool sceneContentBoundsValid = false;
	vec2 sceneContentMin{};
	vec2 sceneContentMax{};

	QPointer<MotionCraftDialog> dialog;
	obs_source_t *markerSource = nullptr;

	int followToggleHotkeyVk = 0;
	bool followToggleHkValid = false;
	bool followToggleModCtrl = false;
	bool followToggleModAlt = false;
	bool followToggleModShift = false;
	bool followToggleModWin = false;

#ifdef _WIN32
	static LRESULT CALLBACK kb_hook_proc(int nCode, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK mouse_hook_proc(int nCode, WPARAM wParam, LPARAM lParam);

	void *keyboardHook = nullptr;
	void *mouseHook = nullptr;
#elif defined(__APPLE__)
	static CGEventRef eventTapCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon);
	CFMachPortRef eventTap = nullptr;
	CFRunLoopSourceRef runLoopSource = nullptr;
#elif defined(__linux__)
	_XDisplay *xiDisplay = nullptr;
	int xiOpcode = 0;
	QSocketNotifier *xiNotifier = nullptr;
	void processXInput2Events();
#endif
};
