#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QKeySequenceEdit;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTabWidget;
struct calldata;

class MotionCraftDialog final : public QDialog {
	Q_OBJECT

public:
	explicit MotionCraftDialog(QWidget *parent = nullptr);

protected:
	void closeEvent(QCloseEvent *event) override;

private slots:
	void refreshLists();
	void applyToController();
	void testZoom();
	void clearFollowToggleHotkey();
	void clearPluginToggleHotkey();
	void syncPluginEnabledFromController();
	void chooseMarkerColor();
	void populateSourcesTab();

private:
	void buildUi();
	void buildZoomLevelsTab();
	void buildWiggleTab();
	void loadFromController();
	void populateSources();
	void updateMarkerColorButton(const QColor &color);

	static void obsSourceChanged(void *data, struct calldata *cd);

	QTabWidget *tabWidget = nullptr;

	QComboBox *cmbSource = nullptr;
	QKeySequenceEdit *editFollowToggleHotkey = nullptr;
	QPushButton *btnClearFollowToggleHotkey = nullptr;
	QCheckBox *chkPluginEnabled = nullptr;
	QKeySequenceEdit *editPluginToggleHotkey = nullptr;
	QPushButton *btnClearPluginToggleHotkey = nullptr;

	struct LevelRow {
		QDoubleSpinBox *zoom = nullptr;
		QSpinBox *in = nullptr;
		QSpinBox *out = nullptr;
		QKeySequenceEdit *hotkey = nullptr;
		QPushButton *clear = nullptr;
	};
	LevelRow levelRows[5];
	QCheckBox *chkHotkeysEnabled = nullptr;

	QCheckBox *chkFollow = nullptr;
	QDoubleSpinBox *spFollowSpeed = nullptr;
	QCheckBox *chkCenterCursorUntilEdge = nullptr;
	QSpinBox *spMouseIdleTimeout = nullptr;
	QCheckBox *chkPortraitCover = nullptr;
	QCheckBox *chkShowCursorMarker = nullptr;
	QSpinBox *spMarkerSize = nullptr;
	QSpinBox *spMarkerThickness = nullptr;
	QPushButton *btnMarkerColor = nullptr;
	QCheckBox *chkDebug = nullptr;

	QCheckBox *chkWiggleEnabled = nullptr;
	QDoubleSpinBox *spWigglePosition = nullptr;
	QDoubleSpinBox *spWiggleRotation = nullptr;
	QDoubleSpinBox *spWiggleScale = nullptr;
	QDoubleSpinBox *spWiggleSpeedMin = nullptr;
	QDoubleSpinBox *spWiggleSpeedMax = nullptr;
	QSpinBox *spWiggleSeed = nullptr;

	QListWidget *lstSources = nullptr;

	QLabel *lblStatus = nullptr;
	QPushButton *btnRefresh = nullptr;
	QPushButton *btnApply = nullptr;
	QPushButton *btnTest = nullptr;

	bool loading = false;
};
