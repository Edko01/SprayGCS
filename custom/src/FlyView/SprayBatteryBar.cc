#include "SprayBatteryBar.h"

#include "BatteryFactGroupListModel.h"
#include "Fact.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

#include <QtCore/QtMath>

namespace {

constexpr qint64 kWindowMs    = 180 * 1000;   // drain rate from the last 3 minutes...
constexpr qint64 kMinWindowMs = 30 * 1000;    // ...once there are at least 30 s of them
constexpr double kMinDropPct  = 1.0;          // and the level has dropped at least 1 %

double parameterValue(Vehicle *vehicle, const char *name, double fallback)
{
    ParameterManager *params = vehicle->parameterManager();
    const QString     key    = QString::fromLatin1(name);
    if (!params || !params->parametersReady() || !params->parameterExists(ParameterManager::defaultComponentId, key)) {
        return fallback;
    }
    Fact *fact = params->getParameter(ParameterManager::defaultComponentId, key);
    return fact ? fact->rawValue().toDouble() : fallback;
}

} // namespace

SprayBatteryBar::SprayBatteryBar(QObject *parent)
    : QObject(parent)
{
    _clock.start();
    _timer.setInterval(1000);
    connect(&_timer, &QTimer::timeout, this, &SprayBatteryBar::_update);
    _timer.start();
}

void SprayBatteryBar::_readFailsafeLevels(Vehicle *vehicle)
{
    _returnPercent = 0.0;
    _landPercent   = 0.0;
    _returnAction.clear();
    _landAction.clear();
    if (!vehicle->px4Firmware()) {
        return;
    }
    // PX4: low = warning only; critical = COM_LOW_BAT_ACT's action; emergency = land.
    // COM_LOW_BAT_ACT: 0 warning, 2 land, 3 return at critical and land at emergency.
    const int action = qRound(parameterValue(vehicle, "COM_LOW_BAT_ACT", -1));
    if (action < 0) {
        return;
    }
    _returnPercent = 100.0 * parameterValue(vehicle, "BAT_CRIT_THR", 0.0);
    _landPercent   = 100.0 * parameterValue(vehicle, "BAT_EMERGEN_THR", 0.0);
    _returnAction  = action == 3 ? tr("RTL") : (action == 2 ? tr("Land") : tr("Warn"));
    _landAction    = action == 0 ? tr("Warn") : tr("Land");
}

double SprayBatteryBar::_drainPerSecond() const
{
    if (_samples.size() < 2) {
        return 0.0;
    }
    const auto  &first = _samples.front();
    const auto  &last  = _samples.back();
    const qint64 ms    = last.first - first.first;
    const double drop  = first.second - last.second;
    if (ms < kMinWindowMs || drop < kMinDropPct) {
        return 0.0;
    }
    return drop / (ms / 1000.0);
}

void SprayBatteryBar::_update()
{
    Vehicle *vehicle = MultiVehicleManager::instance()->activeVehicle();
    if (vehicle != _vehicle) {
        _vehicle = vehicle;
        _samples.clear();
    }

    bool   haveLevel = false;
    double level     = 0.0;
    double reportedSeconds = qQNaN();
    if (vehicle && vehicle->batteries() && vehicle->batteries()->count() > 0) {
        if (auto *battery = qobject_cast<BatteryFactGroup *>(vehicle->batteries()->get(0))) {
            level     = battery->percentRemaining()->rawValue().toDouble();
            haveLevel = !qIsNaN(level) && level >= 0.0 && level <= 100.0;
            reportedSeconds = battery->timeRemaining()->rawValue().toDouble();
        }
    }

    if (haveLevel) {
        _readFailsafeLevels(vehicle);
        // Recent levels while armed; a fresh battery (level jumps up) starts over.
        const qint64 now = _clock.elapsed();
        if (!vehicle->armed() || (!_samples.empty() && level > _samples.back().second + 2.0)) {
            _samples.clear();
        }
        if (vehicle->armed()) {
            _samples.emplace_back(now, level);
            while (!_samples.empty() && now - _samples.front().first > kWindowMs) {
                _samples.pop_front();
            }
        }
    } else {
        _samples.clear();
    }

    int toReturn = -1;
    if (haveLevel && _returnPercent > 0.0) {
        const double above = qMax(0.0, level - _returnPercent);
        const double drain = _drainPerSecond();
        if (drain > 0.0) {
            toReturn = qRound(above / drain);
        } else if (!qIsNaN(reportedSeconds) && reportedSeconds > 0.0 && level > 0.0) {
            // PX4's own estimate is to empty: scale it to the critical level.
            toReturn = qRound(reportedSeconds * above / level);
        }
    }

    const bool notify = haveLevel || haveLevel != _valid;
    _valid           = haveLevel;
    _percent         = level;
    _secondsToReturn = toReturn;
    if (notify) {
        emit changed();   // once a second while there's a level to show
    }
}
