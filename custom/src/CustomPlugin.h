#pragma once

#include <QtCore/QTranslator>
#include <QtQml/QQmlAbstractUrlInterceptor>

#include "QGCCorePlugin.h"
#include "QGCOptions.h"

class ComplexMissionItem;
class PlanCreator;

class CustomOptions;
class CustomPlugin;
class QQmlApplicationEngine;

Q_DECLARE_LOGGING_CATEGORY(CustomLog)

class CustomFlyViewOptions : public QGCFlyViewOptions
{
    Q_OBJECT

public:
    explicit CustomFlyViewOptions(CustomOptions *options, QObject *parent = nullptr);

    // Overrides from CustomFlyViewOptions

    /// This custom build has it's own custom instrument panel. Don't show regular one.
    bool showInstrumentPanel() const final { return false; }
    /// This custom build does not support conecting multiple vehicles to it.
    /// This in turn simplifies various parts of the QGC ui.
    bool showMultiVehicleList() const final { return false; }
};

/*===========================================================================*/

class CustomOptions : public QGCOptions
{
    Q_OBJECT

public:
    explicit CustomOptions(CustomPlugin *plugin, QObject *parent = nullptr);

    // Overrides from QGCOptions

    /// Firmware upgrade page is only shown in Advanced Mode.
    bool showFirmwareUpgrade() const final { return _plugin->showAdvancedUI(); }
    QGCFlyViewOptions *flyViewOptions() const final { return _flyViewOptions; }
    /// Use the in-app file picker everywhere (large, readable, same as on the
    /// tablet) instead of the small system dialog on desktop.
    bool useMobileFileDialog() const final { return true; }
    /// No terrain profile / mission stats panel under the Plan map: the drone
    /// flies a set height (terrain following is onboard) and the job panel has the numbers.
    bool showMissionStatus() const final { return false; }

private:
    QGCCorePlugin *_plugin = nullptr;
    CustomFlyViewOptions *_flyViewOptions = nullptr;
};

/*===========================================================================*/

class CustomPlugin : public QGCCorePlugin
{
    Q_OBJECT

    /// The Plan view's Spray Area (the Fly view shows its field and sprayed trail), or null.
    Q_PROPERTY(QObject *sprayPlanArea READ sprayPlanArea NOTIFY sprayPlanAreaChanged)
    /// Battery level and failsafe levels for the Fly view's battery bar.
    Q_PROPERTY(QObject *sprayBatteryBar READ sprayBatteryBar CONSTANT)

public:
    explicit CustomPlugin(QObject *parent = nullptr);

    static QGCCorePlugin *instance();

    /// Fly view Return: for a mode A spray plan, SprayGCS flies the way back
    /// (through the entry side, then straight to takeoff). Returns an empty
    /// string if it did, otherwise why not (then use the drone's own Return).
    Q_INVOKABLE QString sprayReturn();

    /// On the ground after an interruption (refill, battery swap): plan what's
    /// left of the job from its breakpoint and upload it. Says what happened in
    /// an app message; returns an empty string if it uploaded, otherwise why not.
    Q_INVOKABLE QString sprayResumeJob();

    QObject *sprayPlanArea() const;
    QObject *sprayBatteryBar() const;
    /// Called by a Plan view Spray Area when it's created.
    void sprayPlanAreaCreated(QObject *area);

signals:
    void sprayPlanAreaChanged();

public:

    // Overrides from QGCCorePlugin

    QGCOptions *options() final { return _options; }
    /// This allows you to override/hide QGC Application settings
    void adjustSettingMetaData(const QString &settingsGroup, FactMetaData &metaData, bool &userVisible) final;
    /// This modifies QGC colors palette to match possible custom corporate branding
    void paletteOverride(const QString &colorName, QGCPalette::PaletteColorInfo_t &colorInfo) final;
    /// We override this so we can get access to QQmlApplicationEngine and use it to register our qml module
    QQmlApplicationEngine *createQmlApplicationEngine(QObject *parent) final;
    /// Releases the url interceptor attached in createQmlApplicationEngine before the engine is destroyed
    void destroyQmlApplicationEngine(QQmlApplicationEngine *qmlEngine) final;

    /// Adds the Perimeter Scan item to the complex-item menu.
    QVariantList complexMissionItemNames(Vehicle *vehicle) final;
    /// Factory: creates PerimeterScanComplexItem for our custom type, falls back to base for built-ins.
    ComplexMissionItem *createComplexMissionItem(const QString &complexItemType,
                                                 PlanMasterController *masterController,
                                                 bool flyView,
                                                 const QString &kmlOrShpFile = QString()) final;
    /// Adds the Perimeter Scan plan creator to the New Plan dialog.
    QList<PlanCreator *> planCreators(PlanMasterController *planMasterController) final;
    /// Registers the CustomSettings group so the generated Custom settings page can access it.
    void registerCustomSettings(SettingsManager *settingsManager) final;

private slots:
    void _advancedChanged(bool advanced);

private:
    QString _sprayResumeJob();   ///< sprayResumeJob() without the message

    CustomOptions *_options = nullptr;
    mutable class SprayBatteryBar *_batteryBar = nullptr;   ///< made when the Fly view first asks
    QQmlApplicationEngine *_qmlEngine = nullptr;
    class CustomOverrideInterceptor *_urlInterceptor = nullptr;
};

/*===========================================================================*/

class CustomOverrideInterceptor : public QQmlAbstractUrlInterceptor
{
public:
    CustomOverrideInterceptor();

    QUrl intercept(const QUrl &url, QQmlAbstractUrlInterceptor::DataType type) final;
};
