#include "motioncraft-dialog.hpp"
#include "motioncraft-controller.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>

extern "C" {
struct calldata;
struct signal_handler;
void signal_handler_connect(struct signal_handler *handler, const char *signal,
			    void (*callback)(void *, struct calldata *), void *data);
void signal_handler_disconnect(struct signal_handler *handler, const char *signal,
			       void (*callback)(void *, struct calldata *), void *data);
}

#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QListWidget>
#include <QMap>
#include <QMetaObject>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QString>

namespace {

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static QWidget *mkField(const QString &labelText, QWidget *input)
{
	auto *container = new QWidget;
	auto *v = new QVBoxLayout(container);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(4);
	auto *lbl = new QLabel(labelText, container);
	v->addWidget(lbl);
	v->addWidget(input);
	return container;
}

static void addSection(QVBoxLayout *lay, const QString &title, bool firstSection = false)
{
	if (!firstSection) {
		auto *sep = new QFrame;
		sep->setFrameShape(QFrame::HLine);
		sep->setFrameShadow(QFrame::Sunken);
		lay->addSpacing(12);
		lay->addWidget(sep);
	}
	lay->addSpacing(14);
	auto *lbl = new QLabel(QStringLiteral("<b>%1</b>").arg(title));
	lay->addWidget(lbl);
	lay->addSpacing(10);
}

static void frontend_event_cb(enum obs_frontend_event, void *data)
{
	auto *dlg = static_cast<MotionCraftDialog *>(data);
	if (!dlg)
		return;
	QMetaObject::invokeMethod(dlg, "refreshLists", Qt::QueuedConnection);
}

static bool key_sequence_is_modifier_only(const QKeySequence &seq)
{
	if (seq.isEmpty())
		return false;
	switch (seq[0].key()) {
	case Qt::Key_Control:
	case Qt::Key_Shift:
	case Qt::Key_Alt:
	case Qt::Key_Meta:
		return true;
	default:
		return false;
	}
}

static QString friendlySourceKind(const QString &kind)
{
	if (kind == "image_source")
		return T("SourceKind.Image");
	if (kind == "browser_source")
		return T("SourceKind.Browser");
	if (kind == "game_capture")
		return T("SourceKind.GameCapture");
	if (kind == "window_capture")
		return T("SourceKind.WindowCapture");
	if (kind == "monitor_capture" || kind == "display_capture" || kind == "screen_capture")
		return T("SourceKind.DisplayCapture");
	if (kind == "dshow_input" || kind == "av_capture_input" || kind == "video_capture_device")
		return T("SourceKind.VideoCapture");
	if (kind == "wasapi_input_capture" || kind == "coreaudio_input_capture")
		return T("SourceKind.AudioInput");
	if (kind == "wasapi_output_capture" || kind == "coreaudio_output_capture")
		return T("SourceKind.AudioOutput");
	if (kind == "scene")
		return T("SourceKind.Scene");
	if (kind == "group")
		return T("SourceKind.Group");
	if (kind == "text_gdiplus" || kind == "text_ft2_source")
		return T("SourceKind.Text");
	if (kind == "color_source")
		return T("SourceKind.Color");
	if (kind == "ffmpeg_source")
		return T("SourceKind.Media");
	if (kind == "vlc_source")
		return T("SourceKind.VlcMedia");
	if (kind == "slideshow")
		return T("SourceKind.ImageSlideshow");
	return kind;
}

} // namespace

MotionCraftDialog::MotionCraftDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(T("MotionCraft"));
	setModal(false);
	resize(620, 560);

	buildUi();

	obs_frontend_add_event_callback(frontend_event_cb, this);

	auto *sh = obs_get_signal_handler();
	signal_handler_connect(sh, "source_create", &MotionCraftDialog::obsSourceChanged, this);
	signal_handler_connect(sh, "source_destroy", &MotionCraftDialog::obsSourceChanged, this);
	signal_handler_connect(sh, "source_rename", &MotionCraftDialog::obsSourceChanged, this);

	refreshLists();
	loadFromController();

	/* Escape does not send a close event. QDialog::reject() hides the dialog
	 * and, with WA_DeleteOnClose, deletes it - so hanging the teardown off
	 * closeEvent alone left OBS holding a pointer to a freed dialog, and the
	 * next frontend event dereferenced it. Applying here keeps Escape
	 * behaving like the window's close button, which has always applied. */
	connect(this, &QDialog::finished, this, &MotionCraftDialog::detach);
}

MotionCraftDialog::~MotionCraftDialog()
{
	/* Last line of defence. Whatever destroyed this dialog - Escape, the
	 * close button, or the OBS main window taking its children with it -
	 * OBS must not be left holding a callback into freed memory. */
	unregisterCallbacks();
}

/* Both idempotent: several paths lead here, and on some of them more than one
 * fires. Registering happens once in the constructor, so unregistering must
 * happen exactly once too. */
void MotionCraftDialog::unregisterCallbacks()
{
	if (!callbacksRegistered)
		return;
	callbacksRegistered = false;

	obs_frontend_remove_event_callback(frontend_event_cb, this);

	auto *sh = obs_get_signal_handler();
	signal_handler_disconnect(sh, "source_create", &MotionCraftDialog::obsSourceChanged, this);
	signal_handler_disconnect(sh, "source_destroy", &MotionCraftDialog::obsSourceChanged, this);
	signal_handler_disconnect(sh, "source_rename", &MotionCraftDialog::obsSourceChanged, this);
}

void MotionCraftDialog::detach()
{
	if (detached)
		return;
	detached = true;

	unregisterCallbacks();
	applyToController();
}

void MotionCraftDialog::closeEvent(QCloseEvent *event)
{
	detach();
	QDialog::closeEvent(event);
}

void MotionCraftDialog::obsSourceChanged(void *data, struct calldata *)
{
	auto *dlg = static_cast<MotionCraftDialog *>(data);
	if (dlg)
		QMetaObject::invokeMethod(dlg, "populateSourcesTab", Qt::QueuedConnection);
}

void MotionCraftDialog::buildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(12, 12, 12, 12);
	root->setSpacing(8);

	tabWidget = new QTabWidget(this);

	{
		auto *page = new QWidget;
		auto *lay = new QVBoxLayout(page);
		lay->setContentsMargins(20, 20, 20, 20);
		lay->setSpacing(0);

		chkPluginEnabled = new QCheckBox(T("Dialog.PluginEnabled"), page);
		chkPluginEnabled->setToolTip(T("Dialog.PluginEnabledTooltip"));
		lay->addWidget(chkPluginEnabled);

		auto *startsOff = new QLabel(T("Dialog.PluginStartsOffHelp"), page);
		startsOff->setWordWrap(true);
		lay->addWidget(startsOff);
		lay->addSpacing(8);

		auto *pluginHkRow = new QWidget(page);
		{
			auto *h = new QHBoxLayout(pluginHkRow);
			h->setContentsMargins(0, 0, 0, 0);
			h->setSpacing(6);
			editPluginToggleHotkey = new QKeySequenceEdit(pluginHkRow);
			editPluginToggleHotkey->setToolTip(T("Dialog.PluginToggleHotkeyTooltip"));
			btnClearPluginToggleHotkey = new QPushButton(T("Dialog.Clear"), pluginHkRow);
			btnClearPluginToggleHotkey->setToolTip(T("Dialog.ClearPluginToggleTooltip"));
			h->addWidget(editPluginToggleHotkey, 1);
			h->addWidget(btnClearPluginToggleHotkey);
		}
		lay->addWidget(mkField(T("Dialog.PluginToggleHotkey"), pluginHkRow));
		lay->addSpacing(16);

		cmbSource = new QComboBox(page);
		lay->addWidget(mkField(T("Dialog.TargetScreen"), cmbSource));
		lay->addSpacing(16);

		auto *followHkRow = new QWidget(page);
		{
			auto *h = new QHBoxLayout(followHkRow);
			h->setContentsMargins(0, 0, 0, 0);
			h->setSpacing(6);
			editFollowToggleHotkey = new QKeySequenceEdit(followHkRow);
			btnClearFollowToggleHotkey = new QPushButton(T("Dialog.Clear"), followHkRow);
			btnClearFollowToggleHotkey->setToolTip(T("Dialog.ClearFollowToggleTooltip"));
			h->addWidget(editFollowToggleHotkey, 1);
			h->addWidget(btnClearFollowToggleHotkey);
		}
		lay->addWidget(mkField(T("Dialog.FollowToggleHotkey"), followHkRow));

		lay->addStretch(1);
		tabWidget->addTab(page, T("Dialog.Tab.Target"));
	}

	buildZoomLevelsTab();
	buildWiggleTab();

	{
		auto *page = new QWidget;
		auto *lay = new QVBoxLayout(page);
		lay->setContentsMargins(20, 8, 20, 20);
		lay->setSpacing(0);

		addSection(lay, T("Dialog.Section.MouseFollow"), true);

		chkFollow = new QCheckBox(T("Dialog.Enable"), page);
		chkFollow->setToolTip(T("Dialog.FollowTooltip"));

		spFollowSpeed = new QDoubleSpinBox(page);
		spFollowSpeed->setRange(0.1, 40.0);
		spFollowSpeed->setSingleStep(0.5);
		spFollowSpeed->setDecimals(1);

		chkCenterCursorUntilEdge = new QCheckBox(T("Dialog.CenterCursorUntilEdge"), page);
		chkCenterCursorUntilEdge->setToolTip(T("Dialog.CenterCursorUntilEdgeTooltip"));

		spMouseIdleTimeout = new QSpinBox(page);
		spMouseIdleTimeout->setRange(0, 60000);
		spMouseIdleTimeout->setSingleStep(100);
		spMouseIdleTimeout->setSuffix(T("Unit.Milliseconds"));
		spMouseIdleTimeout->setSpecialValueText(T("Dialog.Disabled"));
		spMouseIdleTimeout->setToolTip(T("Dialog.MouseIdleTimeoutTooltip"));

		cmbFollowSource = new QComboBox(page);
		cmbFollowSource->addItem(T("Dialog.FollowSource.Screen"), false);
		cmbFollowSource->addItem(T("Dialog.FollowSource.Preview"), true);
		cmbFollowSource->setToolTip(T("Dialog.FollowSourceTooltip"));
		lay->addWidget(mkField(T("Dialog.FollowSource"), cmbFollowSource));
		lay->addSpacing(10);

		auto *followRow = new QHBoxLayout;
		followRow->setSpacing(12);

		auto *followEnableW = new QWidget(page);
		{
			auto *v = new QVBoxLayout(followEnableW);
			v->setContentsMargins(0, 0, 0, 0);
			v->setSpacing(4);
			v->addWidget(new QLabel(T("Dialog.FollowMouse"), followEnableW));
			v->addWidget(chkFollow);
		}
		followRow->addWidget(followEnableW, 1);
		followRow->addWidget(mkField(T("Dialog.SmoothingSpeed"), spFollowSpeed), 1);
		followRow->addWidget(mkField(T("Dialog.MouseIdleTimeout"), spMouseIdleTimeout), 1);
		lay->addLayout(followRow);
		lay->addWidget(chkCenterCursorUntilEdge);

		chkNormaliseFollowRange = new QCheckBox(T("Dialog.NormaliseFollowRange"), page);
		chkNormaliseFollowRange->setToolTip(T("Dialog.NormaliseFollowRangeTooltip"));
		lay->addWidget(chkNormaliseFollowRange);

		addSection(lay, T("Dialog.Section.Canvas"));

		chkPortraitCover = new QCheckBox(T("Dialog.PortraitCover"), page);
		chkPortraitCover->setToolTip(T("Dialog.PortraitCoverTooltip"));
		lay->addWidget(chkPortraitCover);

		addSection(lay, T("Dialog.Section.CursorHalo"));

		chkShowCursorMarker = new QCheckBox(T("Dialog.ShowCursorHalo"), page);
		chkShowCursorMarker->setToolTip(T("Dialog.ShowCursorHaloTooltip"));

		auto *haloFlagsRow = new QHBoxLayout;
		haloFlagsRow->setSpacing(20);
		haloFlagsRow->addWidget(chkShowCursorMarker);
		haloFlagsRow->addStretch(1);
		lay->addLayout(haloFlagsRow);
		lay->addSpacing(10);

		spMarkerSize = new QSpinBox(page);
		spMarkerSize->setRange(6, 256);
		spMarkerSize->setSingleStep(2);
		spMarkerSize->setSuffix(T("Unit.Pixels"));

		spMarkerThickness = new QSpinBox(page);
		spMarkerThickness->setRange(1, 64);
		spMarkerThickness->setSingleStep(1);
		spMarkerThickness->setSuffix(T("Unit.Pixels"));

		btnMarkerColor = new QPushButton(page);
		btnMarkerColor->setMinimumHeight(28);

		auto *haloRow = new QHBoxLayout;
		haloRow->setSpacing(12);
		haloRow->addWidget(mkField(T("Dialog.HaloSize"), spMarkerSize), 1);
		haloRow->addWidget(mkField(T("Dialog.RingThickness"), spMarkerThickness), 1);
		haloRow->addWidget(mkField(T("Dialog.Color"), btnMarkerColor), 1);
		lay->addLayout(haloRow);

		addSection(lay, T("Dialog.Section.Developer"));

		chkDebug = new QCheckBox(T("Dialog.EnableDebugLogging"), page);
		lay->addWidget(chkDebug);

		lay->addStretch(1);
		tabWidget->addTab(page, T("Dialog.Tab.Advanced"));
	}

	{
		auto *page = new QWidget;
		auto *lay = new QVBoxLayout(page);
		lay->setContentsMargins(20, 20, 20, 20);
		lay->setSpacing(10);

		auto *info = new QLabel(T("Dialog.SourcesHelp"), page);
		info->setWordWrap(true);
		lay->addWidget(info);

		lstSources = new QListWidget(page);
		lstSources->setAlternatingRowColors(true);
		lstSources->setSortingEnabled(false);
		lay->addWidget(lstSources, 1);

		tabWidget->addTab(page, T("Dialog.Tab.Sources"));
	}

	root->addWidget(tabWidget, 1);

	lblStatus = new QLabel(this);
	lblStatus->setWordWrap(true);
	lblStatus->setText(T("Dialog.StatusTip"));
	root->addWidget(lblStatus);

	auto *btnRow = new QHBoxLayout;
	btnRow->setSpacing(8);
	btnRefresh = new QPushButton(T("Dialog.RefreshLists"), this);
	btnApply = new QPushButton(T("Dialog.Apply"), this);
	btnTest = new QPushButton(T("Dialog.Test"), this);
	btnRow->addWidget(btnRefresh);
	btnRow->addStretch(1);
	btnRow->addWidget(btnTest);
	btnRow->addWidget(btnApply);
	root->addLayout(btnRow);

	connect(btnRefresh, &QPushButton::clicked, this, &MotionCraftDialog::refreshLists);
	connect(btnApply, &QPushButton::clicked, this, &MotionCraftDialog::applyToController);
	connect(btnTest, &QPushButton::clicked, this, &MotionCraftDialog::testZoom);
	connect(btnClearFollowToggleHotkey, &QPushButton::clicked, this, &MotionCraftDialog::clearFollowToggleHotkey);
	connect(btnClearPluginToggleHotkey, &QPushButton::clicked, this, &MotionCraftDialog::clearPluginToggleHotkey);
	/* A switch, not a setting: it takes effect where it is clicked rather than
	 * waiting for Apply, because the whole point of it is to stop the plugin
	 * touching anything right now. */
	connect(chkPluginEnabled, &QCheckBox::toggled, this, [this](bool on) {
		if (loading)
			return;
		MotionCraftController::instance().setPluginEnabled(on);
		lblStatus->setText(on ? T("Dialog.PluginEnabledStatus") : T("Dialog.PluginDisabledStatus"));
	});
	/* The toggle key can flip this while the dialog is open, so the checkbox
	 * follows the controller rather than only being read at Apply. */
	connect(&MotionCraftController::instance(), &MotionCraftController::settingsChanged, this,
		&MotionCraftDialog::syncPluginEnabledFromController);
	connect(btnMarkerColor, &QPushButton::clicked, this, &MotionCraftDialog::chooseMarkerColor);
}

void MotionCraftDialog::buildZoomLevelsTab()
{
	auto *page = new QWidget;
	auto *lay = new QVBoxLayout(page);
	lay->setContentsMargins(20, 16, 20, 20);
	lay->setSpacing(10);

	chkHotkeysEnabled = new QCheckBox(T("Dialog.HotkeysEnabled"), page);
	chkHotkeysEnabled->setToolTip(T("Dialog.HotkeysEnabledTooltip"));
	lay->addWidget(chkHotkeysEnabled);

	auto *info = new QLabel(T("Dialog.ZoomLevels.Help"), page);
	info->setWordWrap(true);
	lay->addWidget(info);

	auto *grid = new QGridLayout;
	grid->setHorizontalSpacing(10);
	grid->setVerticalSpacing(6);

	const QString headers[] = {T("Dialog.ZoomLevels.Level"), T("Dialog.ZoomFactor"), T("Dialog.AnimateIn"),
				   T("Dialog.AnimateOut"), T("Dialog.Hotkey")};
	for (int col = 0; col < 5; ++col) {
		auto *lbl = new QLabel(QStringLiteral("<b>%1</b>").arg(headers[col]), page);
		grid->addWidget(lbl, 0, col);
	}

	for (int i = 0; i < MotionCraftController::kZoomLevelCount; ++i) {
		LevelRow &row = levelRows[i];
		const int r = i + 1;

		grid->addWidget(new QLabel(QString::number(r), page), r, 0);

		row.zoom = new QDoubleSpinBox(page);
		row.zoom->setRange(1.0, 8.0);
		row.zoom->setSingleStep(0.05);
		row.zoom->setDecimals(2);
		row.zoom->setToolTip(T("Dialog.ZoomFactorTooltip"));
		grid->addWidget(row.zoom, r, 1);

		row.in = new QSpinBox(page);
		row.in->setRange(0, 10000);
		row.in->setSingleStep(50);
		row.in->setSuffix(T("Unit.Milliseconds"));
		row.in->setToolTip(T("Dialog.ZoomLevels.AnimateInTooltip"));
		grid->addWidget(row.in, r, 2);

		row.out = new QSpinBox(page);
		row.out->setRange(0, 10000);
		row.out->setSingleStep(50);
		row.out->setSuffix(T("Unit.Milliseconds"));
		row.out->setToolTip(T("Dialog.ZoomLevels.AnimateOutTooltip"));
		grid->addWidget(row.out, r, 3);

		row.hotkey = new QKeySequenceEdit(page);
		grid->addWidget(row.hotkey, r, 4);

		row.clear = new QPushButton(T("Dialog.Clear"), page);
		row.clear->setToolTip(T("Dialog.ZoomLevels.ClearHotkeyTooltip"));
		grid->addWidget(row.clear, r, 5);

		connect(row.clear, &QPushButton::clicked, this,
			[this, i]() { levelRows[i].hotkey->setKeySequence(QKeySequence()); });
	}

	grid->setColumnStretch(4, 1);
	lay->addLayout(grid);

	auto *timing = new QLabel(T("Dialog.ZoomLevels.TimingHelp"), page);
	timing->setWordWrap(true);
	lay->addWidget(timing);

	lay->addStretch(1);
	tabWidget->addTab(page, T("Dialog.Tab.ZoomLevels"));
}

void MotionCraftDialog::buildWiggleTab()
{
	auto *page = new QWidget;
	auto *lay = new QVBoxLayout(page);
	lay->setContentsMargins(20, 16, 20, 20);
	lay->setSpacing(10);

	chkWiggleEnabled = new QCheckBox(T("Dialog.Wiggle.Enable"), page);
	chkWiggleEnabled->setToolTip(T("Dialog.Wiggle.EnableTooltip"));
	lay->addWidget(chkWiggleEnabled);

	auto *info = new QLabel(T("Dialog.Wiggle.Help"), page);
	info->setWordWrap(true);
	lay->addWidget(info);

	addSection(lay, T("Dialog.Wiggle.Section.Amount"));

	spWigglePosition = new QDoubleSpinBox(page);
	spWigglePosition->setRange(0.0, MotionCraftController::kWigglePositionMaxPx);
	spWigglePosition->setSingleStep(0.1);
	spWigglePosition->setDecimals(2);
	spWigglePosition->setSuffix(T("Unit.Pixels"));
	spWigglePosition->setToolTip(T("Dialog.Wiggle.PositionTooltip"));

	spWiggleRotation = new QDoubleSpinBox(page);
	spWiggleRotation->setRange(0.0, MotionCraftController::kWiggleRotationMaxDeg);
	spWiggleRotation->setSingleStep(0.05);
	spWiggleRotation->setDecimals(2);
	spWiggleRotation->setSuffix(T("Unit.Degrees"));
	spWiggleRotation->setToolTip(T("Dialog.Wiggle.RotationTooltip"));

	spWiggleScale = new QDoubleSpinBox(page);
	spWiggleScale->setRange(0.0, MotionCraftController::kWiggleScaleMaxPct);
	spWiggleScale->setSingleStep(0.1);
	spWiggleScale->setDecimals(2);
	spWiggleScale->setSuffix(T("Unit.Percent"));
	spWiggleScale->setToolTip(T("Dialog.Wiggle.ScaleTooltip"));

	auto *amountRow = new QHBoxLayout;
	amountRow->setSpacing(12);
	amountRow->addWidget(mkField(T("Dialog.Wiggle.Position"), spWigglePosition), 1);
	amountRow->addWidget(mkField(T("Dialog.Wiggle.Rotation"), spWiggleRotation), 1);
	amountRow->addWidget(mkField(T("Dialog.Wiggle.Scale"), spWiggleScale), 1);
	lay->addLayout(amountRow);

	addSection(lay, T("Dialog.Wiggle.Section.Speed"));

	spWiggleSpeedMin = new QDoubleSpinBox(page);
	spWiggleSpeedMin->setRange(0.0, MotionCraftController::kWiggleSpeedMax);
	spWiggleSpeedMin->setSingleStep(0.1);
	spWiggleSpeedMin->setDecimals(2);
	spWiggleSpeedMin->setSuffix(T("Unit.Times"));

	spWiggleSpeedMax = new QDoubleSpinBox(page);
	spWiggleSpeedMax->setRange(0.0, MotionCraftController::kWiggleSpeedMax);
	spWiggleSpeedMax->setSingleStep(0.1);
	spWiggleSpeedMax->setDecimals(2);
	spWiggleSpeedMax->setSuffix(T("Unit.Times"));

	spWiggleSeed = new QSpinBox(page);
	spWiggleSeed->setRange(0, 9999);
	spWiggleSeed->setToolTip(T("Dialog.Wiggle.SeedTooltip"));

	auto *speedRow = new QHBoxLayout;
	speedRow->setSpacing(12);
	speedRow->addWidget(mkField(T("Dialog.Wiggle.SpeedMin"), spWiggleSpeedMin), 1);
	speedRow->addWidget(mkField(T("Dialog.Wiggle.SpeedMax"), spWiggleSpeedMax), 1);
	speedRow->addWidget(mkField(T("Dialog.Wiggle.Seed"), spWiggleSeed), 1);
	lay->addLayout(speedRow);

	auto *speedHelp = new QLabel(T("Dialog.Wiggle.SpeedHelp"), page);
	speedHelp->setWordWrap(true);
	lay->addWidget(speedHelp);

	/* Min above max is a state the user passes through while typing, so it is
	 * corrected by nudging the other box rather than by refusing the edit. */
	connect(spWiggleSpeedMin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
		if (!loading && spWiggleSpeedMax->value() < v)
			spWiggleSpeedMax->setValue(v);
	});
	connect(spWiggleSpeedMax, &QDoubleSpinBox::valueChanged, this, [this](double v) {
		if (!loading && spWiggleSpeedMin->value() > v)
			spWiggleSpeedMin->setValue(v);
	});

	lay->addStretch(1);
	tabWidget->addTab(page, T("Dialog.Tab.Wiggle"));
}

void MotionCraftDialog::populateSourcesTab()
{
	if (!lstSources)
		return;

	auto &c = MotionCraftController::instance();

	QSet<QString> included;
	if (lstSources->count() > 0) {
		for (int i = 0; i < lstSources->count(); i++) {
			const QListWidgetItem *it = lstSources->item(i);
			if (it && it->checkState() == Qt::Checked)
				included.insert(it->data(Qt::UserRole).toString());
		}
	} else {
		included = c.includedSources;
	}

	lstSources->clear();

	struct Collector {
		QMap<QString, QString> *map = nullptr;

		static void visitScene(obs_scene_t *scene, Collector *col)
		{
			if (!scene || !col || !col->map)
				return;
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *p) -> bool {
					auto *col2 = static_cast<Collector *>(p);
					if (!item || !col2 || !col2->map)
						return true;

					obs_source_t *src = obs_sceneitem_get_source(item);
					if (!src)
						return true;

					const char *name = obs_source_get_name(src);
					if (!name || !*name)
						return true;

					/* Our own marker, plus any orphan left in a scene by a
					 * pre-rename Zoominator install. Neither is a source
					 * the user can meaningfully pick. */
					if (strcmp(name, "MotionCraft Cursor Marker") == 0 ||
					    strcmp(name, "Zoominator Cursor Marker") == 0)
						return true;

					const QString qname = QString::fromUtf8(name);
					if (!col2->map->contains(qname)) {
						const char *id = obs_source_get_id(src);
						col2->map->insert(qname, id ? QString::fromUtf8(id) : QString());
					}

					if (obs_scene_t *sub = obs_scene_from_source(src))
						Collector::visitScene(sub, col2);

					return true;
				},
				col);
		}
	};

	QMap<QString, QString> sourceMap;
	Collector collector{&sourceMap};

	obs_enum_scenes(
		[](void *p, obs_source_t *sceneSrc) -> bool {
			auto *col = static_cast<Collector *>(p);
			if (sceneSrc && col)
				if (obs_scene_t *scene = obs_scene_from_source(sceneSrc))
					Collector::visitScene(scene, col);
			return true;
		},
		&collector);

	QStringList names = sourceMap.keys();
	names.sort(Qt::CaseInsensitive);

	for (const QString &name : names) {
		const QString kind = friendlySourceKind(sourceMap.value(name));
		const QString label = kind.isEmpty() ? name : QStringLiteral("%1  [%2]").arg(name, kind);

		auto *litem = new QListWidgetItem(label, lstSources);
		litem->setData(Qt::UserRole, name);
		litem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
		litem->setCheckState(included.contains(name) ? Qt::Checked : Qt::Unchecked);
	}
}

void MotionCraftDialog::populateSources()
{
	const QString cur = cmbSource->currentData().toString();

	cmbSource->blockSignals(true);
	cmbSource->clear();
	cmbSource->addItem(T("Dialog.SelectScreen"), "");

	const auto screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); ++i) {
		auto *screen = screens[i];
		if (!screen)
			continue;
		const QRect g = screen->geometry();
		const QString key = QStringLiteral("%1,%2,%3,%4").arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height());
		const QString label = QStringLiteral("%1: %2×%3 @ (%4, %5)")
					      .arg(screen->name())
					      .arg(g.width())
					      .arg(g.height())
					      .arg(g.x())
					      .arg(g.y());
		cmbSource->addItem(label, key);
	}

	int idx = cmbSource->findData(cur);
	if (idx < 0)
		idx = cmbSource->findData(MotionCraftController::instance().screenKey);
	if (idx >= 0)
		cmbSource->setCurrentIndex(idx);

	cmbSource->blockSignals(false);
}

void MotionCraftDialog::refreshLists()
{
	populateSources();
	populateSourcesTab();
}

void MotionCraftDialog::loadFromController()
{
	loading = true;
	auto &c = MotionCraftController::instance();

	refreshLists();

	{
		const int idx = cmbSource->findData(c.screenKey);
		if (idx >= 0)
			cmbSource->setCurrentIndex(idx);

		editFollowToggleHotkey->setKeySequence(QKeySequence(c.followToggleHotkeySequence));
		editPluginToggleHotkey->setKeySequence(QKeySequence(c.pluginToggleHotkeySequence));
		chkPluginEnabled->setChecked(c.pluginEnabled);
	}

	chkHotkeysEnabled->setChecked(c.hotkeysEnabled);

	for (int i = 0; i < MotionCraftController::kZoomLevelCount; ++i) {
		const auto &lv = c.zoomLevels[i];
		levelRows[i].zoom->setValue(lv.zoom);
		levelRows[i].in->setValue(lv.inMs);
		levelRows[i].out->setValue(lv.outMs);
		levelRows[i].hotkey->setKeySequence(QKeySequence(lv.hotkey));
	}

	{
		chkFollow->setChecked(c.followMouse);
		cmbFollowSource->setCurrentIndex(c.followFromPreview ? 1 : 0);
		spFollowSpeed->setValue(c.followSpeed);
		chkCenterCursorUntilEdge->setChecked(c.centerCursorUntilEdge);
		chkNormaliseFollowRange->setChecked(c.normaliseFollowRange);
		spMouseIdleTimeout->setValue(c.mouseIdleTimeoutMs);
		chkPortraitCover->setChecked(c.portraitCover);
		chkShowCursorMarker->setChecked(c.showCursorMarker);
		spMarkerSize->setValue(c.markerSize);
		spMarkerThickness->setValue(c.markerThickness);
		updateMarkerColorButton(QColor::fromRgba(c.markerColor));
		chkDebug->setChecked(c.debug);
	}

	{
		chkWiggleEnabled->setChecked(c.wiggleEnabled);
		spWigglePosition->setValue(c.wigglePositionPx);
		spWiggleRotation->setValue(c.wiggleRotationDeg);
		spWiggleScale->setValue(c.wiggleScalePct);
		spWiggleSpeedMin->setValue(c.wiggleSpeedMin);
		spWiggleSpeedMax->setValue(c.wiggleSpeedMax);
		spWiggleSeed->setValue(c.wiggleSeed);
	}

	if (lstSources)
		lstSources->clear();
	populateSourcesTab();

	loading = false;
}

void MotionCraftDialog::applyToController()
{
	if (loading)
		return;

	auto &c = MotionCraftController::instance();

	c.screenKey = cmbSource->currentData().toString();
	c.followToggleHotkeySequence = editFollowToggleHotkey->keySequence().toString(QKeySequence::NativeText);

	/* A bare modifier would fire on its own constantly, and this key switches
	 * the whole plugin, so drop it rather than store a trigger that cannot be
	 * lived with. */
	const QKeySequence pluginSeq = editPluginToggleHotkey->keySequence();
	c.pluginToggleHotkeySequence =
		key_sequence_is_modifier_only(pluginSeq) ? QString() : pluginSeq.toString(QKeySequence::NativeText);

	c.hotkeysEnabled = chkHotkeysEnabled->isChecked();

	for (int i = 0; i < MotionCraftController::kZoomLevelCount; ++i) {
		auto &lv = c.zoomLevels[i];
		lv.zoom = levelRows[i].zoom->value();
		lv.inMs = levelRows[i].in->value();
		lv.outMs = levelRows[i].out->value();

		/* A bare modifier cannot be a level key - the controller rejects it -
		 * so drop it here rather than silently storing a dead binding. */
		const QKeySequence seq = levelRows[i].hotkey->keySequence();
		lv.hotkey = key_sequence_is_modifier_only(seq) ? QString() : seq.toString(QKeySequence::NativeText);
	}

	c.followMouse = chkFollow->isChecked();
	c.followFromPreview = cmbFollowSource->currentData().toBool();
	c.followSpeed = spFollowSpeed->value();
	c.centerCursorUntilEdge = chkCenterCursorUntilEdge->isChecked();
	c.normaliseFollowRange = chkNormaliseFollowRange->isChecked();
	c.mouseIdleTimeoutMs = spMouseIdleTimeout->value();
	c.portraitCover = chkPortraitCover->isChecked();
	c.showCursorMarker = chkShowCursorMarker->isChecked();
	c.markerOnlyOnClick = true;
	c.markerSize = spMarkerSize->value();
	c.markerThickness = spMarkerThickness->value();
	if (btnMarkerColor)
		c.markerColor = (uint32_t)btnMarkerColor->property("markerRgba").toUInt();
	c.debug = chkDebug->isChecked();

	c.wigglePositionPx = spWigglePosition->value();
	c.wiggleRotationDeg = spWiggleRotation->value();
	c.wiggleScalePct = spWiggleScale->value();
	c.wiggleSpeedMin = spWiggleSpeedMin->value();
	c.wiggleSpeedMax = spWiggleSpeedMax->value();
	c.wiggleSeed = spWiggleSeed->value();

	c.includedSources.clear();
	if (lstSources) {
		for (int i = 0; i < lstSources->count(); i++) {
			const QListWidgetItem *it = lstSources->item(i);
			if (it && it->checkState() == Qt::Checked)
				c.includedSources.insert(it->data(Qt::UserRole).toString());
		}
	}

	/* Last, because switching the wiggle on captures the scene, and that has to
	 * see the included-source list and the amplitudes this Apply just set.
	 * A disabled plugin records the preference without acting on it. */
	c.setWiggleEnabled(chkWiggleEnabled->isChecked());

	c.saveSettings();
	c.rebuildRuntimeHooks();
	lblStatus->setText(T("Dialog.SettingsApplied"));
}

void MotionCraftDialog::testZoom()
{
	applyToController();
	lblStatus->setText(T("Dialog.TestStatus"));
}

void MotionCraftDialog::clearFollowToggleHotkey()
{
	editFollowToggleHotkey->setKeySequence(QKeySequence());
}

void MotionCraftDialog::clearPluginToggleHotkey()
{
	editPluginToggleHotkey->setKeySequence(QKeySequence());
}

void MotionCraftDialog::syncPluginEnabledFromController()
{
	if (!chkPluginEnabled)
		return;
	const bool on = MotionCraftController::instance().pluginEnabled;
	if (chkPluginEnabled->isChecked() == on)
		return;
	const bool wasLoading = loading;
	loading = true;
	chkPluginEnabled->setChecked(on);
	loading = wasLoading;
	lblStatus->setText(on ? T("Dialog.PluginEnabledStatus") : T("Dialog.PluginDisabledStatus"));
}

void MotionCraftDialog::updateMarkerColorButton(const QColor &color)
{
	if (!btnMarkerColor)
		return;
	const QColor c = color.isValid() ? color : QColor(255, 0, 0);
	btnMarkerColor->setProperty("markerRgba", c.rgba());
	btnMarkerColor->setText(QStringLiteral("%1 / %2 / %3").arg(c.red()).arg(c.green()).arg(c.blue()));
	btnMarkerColor->setStyleSheet(QStringLiteral("QPushButton { background:%1; color:%2;"
						     " border:1px solid palette(mid); padding:4px 8px; }")
					      .arg(c.name(QColor::HexArgb))
					      .arg(c.lightness() < 128 ? "#ffffff" : "#000000"));
}

void MotionCraftDialog::chooseMarkerColor()
{
	const uint rgba = btnMarkerColor ? btnMarkerColor->property("markerRgba").toUInt() : QColor(255, 0, 0).rgba();
	const QColor picked = QColorDialog::getColor(QColor::fromRgba(rgba), this, T("Dialog.PickCursorHaloColor"),
						     QColorDialog::ShowAlphaChannel);
	if (picked.isValid())
		updateMarkerColorButton(picked);
}
