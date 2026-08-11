#include "zoominator-dialog.hpp"
#include "zoominator-controller.hpp"

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
	auto *dlg = static_cast<ZoominatorDialog *>(data);
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

ZoominatorDialog::ZoominatorDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(T("Zoominator"));
	setModal(false);
	resize(620, 560);

	buildUi();

	obs_frontend_add_event_callback(frontend_event_cb, this);

	auto *sh = obs_get_signal_handler();
	signal_handler_connect(sh, "source_create", &ZoominatorDialog::obsSourceChanged, this);
	signal_handler_connect(sh, "source_destroy", &ZoominatorDialog::obsSourceChanged, this);
	signal_handler_connect(sh, "source_rename", &ZoominatorDialog::obsSourceChanged, this);

	refreshLists();
	loadFromController();
}

void ZoominatorDialog::closeEvent(QCloseEvent *event)
{
	obs_frontend_remove_event_callback(frontend_event_cb, this);

	auto *sh = obs_get_signal_handler();
	signal_handler_disconnect(sh, "source_create", &ZoominatorDialog::obsSourceChanged, this);
	signal_handler_disconnect(sh, "source_destroy", &ZoominatorDialog::obsSourceChanged, this);
	signal_handler_disconnect(sh, "source_rename", &ZoominatorDialog::obsSourceChanged, this);

	applyToController();
	QDialog::closeEvent(event);
}

void ZoominatorDialog::obsSourceChanged(void *data, struct calldata *)
{
	auto *dlg = static_cast<ZoominatorDialog *>(data);
	if (dlg)
		QMetaObject::invokeMethod(dlg, "populateSourcesTab", Qt::QueuedConnection);
}

void ZoominatorDialog::buildUi()
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

	connect(btnRefresh, &QPushButton::clicked, this, &ZoominatorDialog::refreshLists);
	connect(btnApply, &QPushButton::clicked, this, &ZoominatorDialog::applyToController);
	connect(btnTest, &QPushButton::clicked, this, &ZoominatorDialog::testZoom);
	connect(btnClearFollowToggleHotkey, &QPushButton::clicked, this, &ZoominatorDialog::clearFollowToggleHotkey);
	connect(btnMarkerColor, &QPushButton::clicked, this, &ZoominatorDialog::chooseMarkerColor);
}

void ZoominatorDialog::buildZoomLevelsTab()
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

	for (int i = 0; i < ZoominatorController::kZoomLevelCount; ++i) {
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

void ZoominatorDialog::populateSourcesTab()
{
	if (!lstSources)
		return;

	auto &c = ZoominatorController::instance();

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

					if (strcmp(name, "Zoominator Cursor Marker") == 0)
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

void ZoominatorDialog::populateSources()
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
		idx = cmbSource->findData(ZoominatorController::instance().screenKey);
	if (idx >= 0)
		cmbSource->setCurrentIndex(idx);

	cmbSource->blockSignals(false);
}

void ZoominatorDialog::refreshLists()
{
	populateSources();
	populateSourcesTab();
}

void ZoominatorDialog::loadFromController()
{
	loading = true;
	auto &c = ZoominatorController::instance();

	refreshLists();

	{
		const int idx = cmbSource->findData(c.screenKey);
		if (idx >= 0)
			cmbSource->setCurrentIndex(idx);

		editFollowToggleHotkey->setKeySequence(QKeySequence(c.followToggleHotkeySequence));
	}

	chkHotkeysEnabled->setChecked(c.hotkeysEnabled);

	for (int i = 0; i < ZoominatorController::kZoomLevelCount; ++i) {
		const auto &lv = c.zoomLevels[i];
		levelRows[i].zoom->setValue(lv.zoom);
		levelRows[i].in->setValue(lv.inMs);
		levelRows[i].out->setValue(lv.outMs);
		levelRows[i].hotkey->setKeySequence(QKeySequence(lv.hotkey));
	}

	{
		chkFollow->setChecked(c.followMouse);
		spFollowSpeed->setValue(c.followSpeed);
		chkCenterCursorUntilEdge->setChecked(c.centerCursorUntilEdge);
		spMouseIdleTimeout->setValue(c.mouseIdleTimeoutMs);
		chkPortraitCover->setChecked(c.portraitCover);
		chkShowCursorMarker->setChecked(c.showCursorMarker);
		spMarkerSize->setValue(c.markerSize);
		spMarkerThickness->setValue(c.markerThickness);
		updateMarkerColorButton(QColor::fromRgba(c.markerColor));
		chkDebug->setChecked(c.debug);
	}

	if (lstSources)
		lstSources->clear();
	populateSourcesTab();

	loading = false;
}

void ZoominatorDialog::applyToController()
{
	if (loading)
		return;

	auto &c = ZoominatorController::instance();

	c.screenKey = cmbSource->currentData().toString();
	c.followToggleHotkeySequence = editFollowToggleHotkey->keySequence().toString(QKeySequence::NativeText);

	c.hotkeysEnabled = chkHotkeysEnabled->isChecked();

	for (int i = 0; i < ZoominatorController::kZoomLevelCount; ++i) {
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
	c.followSpeed = spFollowSpeed->value();
	c.centerCursorUntilEdge = chkCenterCursorUntilEdge->isChecked();
	c.mouseIdleTimeoutMs = spMouseIdleTimeout->value();
	c.portraitCover = chkPortraitCover->isChecked();
	c.showCursorMarker = chkShowCursorMarker->isChecked();
	c.markerOnlyOnClick = true;
	c.markerSize = spMarkerSize->value();
	c.markerThickness = spMarkerThickness->value();
	if (btnMarkerColor)
		c.markerColor = (uint32_t)btnMarkerColor->property("markerRgba").toUInt();
	c.debug = chkDebug->isChecked();

	c.includedSources.clear();
	if (lstSources) {
		for (int i = 0; i < lstSources->count(); i++) {
			const QListWidgetItem *it = lstSources->item(i);
			if (it && it->checkState() == Qt::Checked)
				c.includedSources.insert(it->data(Qt::UserRole).toString());
		}
	}

	c.saveSettings();
	c.rebuildRuntimeHooks();
	lblStatus->setText(T("Dialog.SettingsApplied"));
}

void ZoominatorDialog::testZoom()
{
	applyToController();
	lblStatus->setText(T("Dialog.TestStatus"));
}

void ZoominatorDialog::clearFollowToggleHotkey()
{
	editFollowToggleHotkey->setKeySequence(QKeySequence());
}

void ZoominatorDialog::updateMarkerColorButton(const QColor &color)
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

void ZoominatorDialog::chooseMarkerColor()
{
	const uint rgba = btnMarkerColor ? btnMarkerColor->property("markerRgba").toUInt() : QColor(255, 0, 0).rgba();
	const QColor picked = QColorDialog::getColor(QColor::fromRgba(rgba), this, T("Dialog.PickCursorHaloColor"),
						     QColorDialog::ShowAlphaChannel);
	if (picked.isValid())
		updateMarkerColorButton(picked);
}
