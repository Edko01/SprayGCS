#include "CustomPlugin.h"
#include "CustomSettings.h"
#include "PerimeterScanComplexItem.h"
#include "PerimeterScanPlanCreator.h"
#include "SprayAreaComplexItem.h"
#include "FlyView/SprayBatteryBar.h"
#include "SprayAreaPlanCreator.h"
#include "MissionController.h"
#include "PlanMasterController.h"
#include "QmlObjectListModel.h"
#include "AppMessages.h"
#include "QGCLoggingCategory.h"
#include "QGCPalette.h"
#include "QGCMAVLink.h"
#include "AppSettings.h"
#include "SettingsManager.h"
#include "VideoSettings.h"
#include "PlanViewSettings.h"
#include "FlyViewSettings.h"
#include "MultiVehicleManager.h"
#include "Vehicle.h"
#include "ParameterManager.h"
#include "Fact.h"

#include <QtCore/QApplicationStatic>
#include <QtCore/QFile>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlFile>

QGC_LOGGING_CATEGORY(CustomLog, "Custom.CustomPlugin")

Q_APPLICATION_STATIC(CustomPlugin, _customPluginInstance);

CustomFlyViewOptions::CustomFlyViewOptions(CustomOptions* options, QObject* parent)
    : QGCFlyViewOptions(options, parent)
{
    qCDebug(CustomLog) << this;
}

CustomOptions::CustomOptions(CustomPlugin *plugin, QObject *parent)
    : QGCOptions(parent)
    , _plugin(plugin)
    , _flyViewOptions(new CustomFlyViewOptions(this, this))
{
    Q_CHECK_PTR(_plugin);
}

/*===========================================================================*/

CustomPlugin::CustomPlugin(QObject *parent)
    : QGCCorePlugin(parent)
    , _options(new CustomOptions(this, this))
{
    qCDebug(CustomLog) << this;

    _showAdvancedUI = false;
    (void) connect(this, &QGCCorePlugin::showAdvancedUIChanged, this, &CustomPlugin::_advancedChanged);
}

QGCCorePlugin *CustomPlugin::instance()
{
    return _customPluginInstance();
}

void CustomPlugin::_advancedChanged(bool changed)
{
    // Firmware Upgrade page is only show in Advanced mode
    emit _options->showFirmwareUpgradeChanged(changed);
}

void CustomPlugin::registerCustomSettings(SettingsManager *settingsManager)
{
    settingsManager->registerCustomSettingsGroup(QStringLiteral("customSettings"), new CustomSettings());
}

namespace {

// True when running inside WSL (Windows Subsystem for Linux).
[[maybe_unused]] bool runningUnderWsl()
{
    if (qEnvironmentVariableIsSet("WSL_DISTRO_NAME")) {
        return true;
    }
    QFile osRelease(QStringLiteral("/proc/sys/kernel/osrelease"));
    return osRelease.open(QIODevice::ReadOnly)
           && osRelease.readAll().toLower().contains("microsoft");
}

} // namespace

void CustomPlugin::adjustSettingMetaData(const QString& settingsGroup, FactMetaData& metaData, bool &userVisible)
{
    QGCCorePlugin::adjustSettingMetaData(settingsGroup, metaData, userVisible);

    if (settingsGroup == AppSettings::settingsGroup) {
        // This tells QGC than when you are creating Plans while not connected to a vehicle
        // the specific firmware/vehicle the plan is for.
        if (metaData.name() == AppSettings::offlineEditingFirmwareClassName) {
            metaData.setRawDefaultValue(QGCMAVLink::FirmwareClassPX4);
            userVisible = false;
            return;
        } else if (metaData.name() == AppSettings::offlineEditingVehicleClassName) {
            metaData.setRawDefaultValue(QGCMAVLink::VehicleClassMultiRotor);
            userVisible = false;
            return;
        }
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
        else if (metaData.name() == AppSettings::uiScalePercentName) {
            // SprayGCS: start larger on desktop so it's readable without
            // changing settings. (Tablets scale by screen density already;
            // we'll tune that default when we build for Android.)
            metaData.setRawDefaultValue(QVariant(175u));
            return;
        }
#endif
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
        else if (metaData.name() == AppSettings::audioMutedName && runningUnderWsl()) {
            // SprayGCS: voice announcements start muted under WSL. The Windows
            // audio bridge clips short speech to a click/blip at every flight
            // mode change or vehicle message. Turn it back on with the box next to
            // the volume slider in Application Settings > General. The tablet keeps voice.
            metaData.setRawDefaultValue(true);
            return;
        }
#endif
    } else if (settingsGroup == VideoSettings::settingsGroup) {
        // SprayGCS: video starts off. A spray drone doesn't need a camera feed,
        // decoding one costs a lot of CPU (and slows shutdown), and a camera
        // that announces a stream would otherwise turn video on by itself.
        // The pilot can still switch it on (Video Stream Enabled).
        if (metaData.name() == VideoSettings::streamEnabledName) {
            metaData.setRawDefaultValue(false);
            return;
        }
    } else if (settingsGroup == PlanViewSettings::settingsGroup) {
        // SprayGCS: options that don't apply to a quad sprayer are hidden (off):
        // condition gates only change QGC's camera Survey and Corridor Scan
        // patterns; multiple landing patterns are for fixed-wing, and spray plans
        // keep exactly one (the way home for the drone's Return); the VTOL
        // transition distance is for VTOL aircraft.
        if (metaData.name() == PlanViewSettings::useConditionGateName
                || metaData.name() == PlanViewSettings::allowMultipleLandingPatternsName) {
            metaData.setRawDefaultValue(false);
            userVisible = false;
            return;
        }
        if (metaData.name() == PlanViewSettings::vtolTransitionDistanceName) {
            userVisible = false;
            return;
        }
    } else if (settingsGroup == FlyViewSettings::settingsGroup) {
        // SprayGCS: a spray drone has no camera to trigger.
        if (metaData.name() == FlyViewSettings::showSimpleCameraControlName) {
            metaData.setRawDefaultValue(false);
            userVisible = false;
            return;
        }
    }
}

void CustomPlugin::paletteOverride(const QString &colorName, QGCPalette::PaletteColorInfo_t& colorInfo)
{
    if (colorName == QStringLiteral("window")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#212529");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#f8f9fa");
    } else if (colorName == QStringLiteral("windowShade")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#343a40");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#343a40");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#f1f3f5");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#d9d9d9");
    } else if (colorName == QStringLiteral("windowShadeDark")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#1a1c1f");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#1a1c1f");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#e9ecef");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#bdbdbd");
    } else if (colorName == QStringLiteral("text")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#ffffff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#b4b9c3");  // SprayGCS: was #777c89, too dim
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#9d9d9d");
    } else if (colorName == QStringLiteral("warningText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#e03131");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#e03131");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#cc0808");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#cc0808");
    } else if (colorName == QStringLiteral("button")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#495057");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#495057");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#ffffff");
    } else if (colorName == QStringLiteral("buttonText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#ffffff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#777c89");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#9d9d9d");
    } else if (colorName == QStringLiteral("buttonHighlight")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#07916d");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#495057");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#aeebd0");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#e4e4e4");
    } else if (colorName == QStringLiteral("buttonHighlightText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#ffffff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#777c89");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#2c2c2c");
    } else if (colorName == QStringLiteral("primaryButton")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#12b886");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#495057");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#aeebd0");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#585858");
    } else if (colorName == QStringLiteral("primaryButtonText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#ffffff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#cad0d0");
    } else if (colorName == QStringLiteral("textField")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#212529");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#495057");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#f1f3f5");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#ffffff");
    } else if (colorName == QStringLiteral("textFieldText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#ffffff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#b4b9c3");  // SprayGCS: was #777c89
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#808080");
    } else if (colorName == QStringLiteral("mapButton")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#000000");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#585858");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#585858");
    } else if (colorName == QStringLiteral("mapButtonHighlight")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#07916d");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#585858");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#be781c");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#585858");
    } else if (colorName == QStringLiteral("mapIndicator")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#9dda4f");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#585858");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#be781c");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#585858");
    } else if (colorName == QStringLiteral("mapIndicatorChild")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#527942");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#585858");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#766043");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#585858");
    } else if (colorName == QStringLiteral("colorGreen")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#27bf89");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#0ca678");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#009431");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#009431");
    } else if (colorName == QStringLiteral("colorOrange")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#f7b24a");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#f6921e");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#b95604");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#b95604");
    } else if (colorName == QStringLiteral("colorRed")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#e1544c");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#e03131");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#ed3939");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#ed3939");
    } else if (colorName == QStringLiteral("colorGrey")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#8b90a0");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#8b90a0");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#808080");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#808080");
    } else if (colorName == QStringLiteral("colorBlue")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#228be6");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#228be6");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#1a72ff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#1a72ff");
    } else if (colorName == QStringLiteral("alertBackground")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#d4b106");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#d4b106");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#fffb8f");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#b45d48");
    } else if (colorName == QStringLiteral("alertBorder")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#876800");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#876800");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#808080");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#808080");
    } else if (colorName == QStringLiteral("alertText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#000000");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#fff9ed");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#fff9ed");
    } else if (colorName == QStringLiteral("missionItemEditor")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#212529");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#0b1420");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#585858");
    } else if (colorName == QStringLiteral("hoverColor")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#07916d");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#33c494");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#aeebd0");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#464f5a");
    } else if (colorName == QStringLiteral("mapWidgetBorderLight")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#ffffff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#f1f3f5");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#ffffff");
    } else if (colorName == QStringLiteral("mapWidgetBorderDark")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#000000");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#000000");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#000000");
    } else if (colorName == QStringLiteral("brandPrimary")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#4a2c6d");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#4a2c6d");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#4a2c6d");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#4a2c6d");
    } else if (colorName == QStringLiteral("brandAccent")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#6045c5");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#48d6ff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#6045c5");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#48d6ff");
    }
}

QQmlApplicationEngine* CustomPlugin::createQmlApplicationEngine(QObject* parent)
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // WSL workaround: every text field re-reads the clipboard whenever it
    // changes (to know whether Paste is possible), and under WSL each read
    // goes through the slow Windows clipboard bridge. With dozens of fields
    // this freezes the app for minutes, e.g. after copying text in Windows or
    // when closing. Stop the change notifications; Ctrl+V still pastes.
    if (runningUnderWsl() && QGuiApplication::platformName() == QStringLiteral("xcb")) {
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            clipboard->blockSignals(true);
            qCDebug(CustomLog) << "WSL: clipboard change notifications disabled";
        }
    }
#endif

    // The drone's Return (RTL_TYPE, RTL_RETURN_ALT) is set to suit each plan
    // when it's uploaded: see SprayAreaComplexItem::_applyReturnSettings.

    _qmlEngine = QGCCorePlugin::createQmlApplicationEngine(parent);
    _qmlEngine->addImportPath("qrc:/qml/Custom/Widgets");
    _qmlEngine->addImportPath("qrc:/qml/Custom/Plan");
    // TODO: Investigate _qmlEngine->setExtraSelectors({"custom"})

    _urlInterceptor = new CustomOverrideInterceptor();
    _qmlEngine->addUrlInterceptor(_urlInterceptor);

    return _qmlEngine;
}

void CustomPlugin::destroyQmlApplicationEngine(QQmlApplicationEngine *qmlEngine)
{
    if (qmlEngine && (qmlEngine == _qmlEngine)) {
        qmlEngine->removeUrlInterceptor(_urlInterceptor);
        delete _urlInterceptor;
        _urlInterceptor = nullptr;
        _qmlEngine = nullptr;
    }

    QGCCorePlugin::destroyQmlApplicationEngine(qmlEngine);
}

/*===========================================================================*/

CustomOverrideInterceptor::CustomOverrideInterceptor()
    : QQmlAbstractUrlInterceptor()
{

}

QUrl CustomOverrideInterceptor::intercept(const QUrl &url, QQmlAbstractUrlInterceptor::DataType type)
{
    switch (type) {
    case QQmlAbstractUrlInterceptor::QmlFile:
    case QQmlAbstractUrlInterceptor::UrlString:
        if (url.scheme() == QStringLiteral("qrc")) {
            const QString origPath = url.path();
            const QString overrideRes = QStringLiteral(":/Custom%1").arg(origPath);
            if (QFile::exists(overrideRes)) {
                const QString relPath = overrideRes.mid(2);
                QUrl result;
                result.setScheme(QStringLiteral("qrc"));
                result.setPath('/' + relPath);
                return result;
            }
        }
        break;
    default:
        break;
    }

    return url;
}

/*===========================================================================*/

QString CustomPlugin::sprayReturn()
{
    SprayAreaComplexItem *item = SprayAreaComplexItem::planViewItem();
    if (!item) {
        return tr("No spray plan is open.");
    }
    return item->returnViaEntrySide();
}

QString CustomPlugin::sprayResumeJob()
{
    const QString problem = _sprayResumeJob();
    QGC::showAppMessage(problem.isEmpty() ? tr("Uploading what's left of the job. When it's done, start the mission.") : problem);
    return problem;
}

QString CustomPlugin::_sprayResumeJob()
{
    SprayAreaComplexItem *item = SprayAreaComplexItem::planViewItem();
    if (!item) {
        return tr("No spray plan is open.");
    }
    Vehicle *vehicle = MultiVehicleManager::instance()->activeVehicle();
    if (!vehicle) {
        return tr("No drone is connected.");
    }
    if (vehicle->flying()) {
        return tr("Land the drone first.");
    }
    PlanMasterController *master = item->masterController();
    if (!master || master->offline() || master->syncInProgress()) {
        return tr("Can't upload to the drone right now. Try again in a moment.");
    }
    if (item->hasBreakpoint()) {
        item->resumeFromBreakpoint();
    }
    if (item->allSprayed()) {
        return tr("Everything in this field has been sprayed.");
    }
    master->sendToVehicle();
    return QString();
}

QObject *CustomPlugin::sprayBatteryBar() const
{
    if (!_batteryBar) {
        _batteryBar = new SprayBatteryBar(const_cast<CustomPlugin *>(this));
    }
    return _batteryBar;
}

QObject *CustomPlugin::sprayPlanArea() const
{
    return SprayAreaComplexItem::planViewItem();
}

void CustomPlugin::sprayPlanAreaCreated(QObject *area)
{
    if (area) {
        connect(area, &QObject::destroyed, this, &CustomPlugin::sprayPlanAreaChanged);
    }
    emit sprayPlanAreaChanged();
}

QVariantList CustomPlugin::complexMissionItemNames(Vehicle *vehicle)
{
    // Start with the standard set, then append our custom item.
    QVariantList items = QGCCorePlugin::complexMissionItemNames(vehicle);

    QVariantMap entry;
    entry[QStringLiteral("canonicalName")]  = QString(PerimeterScanComplexItem::canonicalName);
    entry[QStringLiteral("translatedName")] = PerimeterScanComplexItem::tr(PerimeterScanComplexItem::canonicalName);
    items.append(entry);

    QVariantMap sprayEntry;
    sprayEntry[QStringLiteral("canonicalName")]  = QString(SprayAreaComplexItem::canonicalName);
    sprayEntry[QStringLiteral("translatedName")] = SprayAreaComplexItem::tr(SprayAreaComplexItem::canonicalName);
    items.prepend(sprayEntry);   // first in the Pattern menu

    return items;
}

ComplexMissionItem *CustomPlugin::createComplexMissionItem(const QString &complexItemType,
                                                            PlanMasterController *masterController,
                                                            bool flyView,
                                                            const QString &kmlOrShpFile)
{
    if (complexItemType == PerimeterScanComplexItem::canonicalName
            || complexItemType == PerimeterScanComplexItem::jsonComplexItemTypeValue) {
        return new PerimeterScanComplexItem(masterController, flyView, kmlOrShpFile);
    }
    if (complexItemType == SprayAreaComplexItem::canonicalName) {
        // Added from the Plan view (Pattern menu, plan creator): one Spray Area
        // per plan. If there already is one, select it instead of adding another.
        MissionController *missionController = masterController ? masterController->missionController() : nullptr;
        QmlObjectListModel *items = missionController ? missionController->visualItems() : nullptr;
        for (int i = 0; items && i < items->count(); ++i) {
            if (auto *existing = items->value<SprayAreaComplexItem *>(i)) {
                missionController->setCurrentPlanViewSeqNum(existing->sequenceNumber(), true);
                QGC::showAppMessage(tr("This plan already has a Spray Area. It has been selected instead of adding another."));
                return nullptr;
            }
        }
        return new SprayAreaComplexItem(masterController, flyView, kmlOrShpFile);
    }
    if (complexItemType == SprayAreaComplexItem::jsonComplexItemTypeValue) {
        // Loading a saved plan.
        return new SprayAreaComplexItem(masterController, flyView, kmlOrShpFile);
    }
    // Fall back to the built-in factory for all standard item types.
    return QGCCorePlugin::createComplexMissionItem(complexItemType, masterController, flyView, kmlOrShpFile);
}

QList<PlanCreator *> CustomPlugin::planCreators(PlanMasterController *planMasterController)
{
    // Start with the standard creators, then add ours.
    QList<PlanCreator *> creators = QGCCorePlugin::planCreators(planMasterController);
    creators.append(new PerimeterScanPlanCreator(planMasterController));
    creators.prepend(new SprayAreaPlanCreator(planMasterController));
    return creators;
}
