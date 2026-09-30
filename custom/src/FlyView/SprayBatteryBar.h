#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include <deque>
#include <utility>

class Vehicle;

/// The data behind the Fly view's battery bar (DJI style): battery level, the
/// levels where PX4's battery failsafes act (Return at critical, Land at
/// emergency, per COM_LOW_BAT_ACT), and the time left until the Return failsafe.
class SprayBatteryBar : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool    valid           READ valid           NOTIFY changed)   ///< a drone is connected and reports its battery level
    Q_PROPERTY(double  percent         READ percent         NOTIFY changed)
    Q_PROPERTY(double  returnPercent   READ returnPercent   NOTIFY changed)   ///< critical level (0: unknown)
    Q_PROPERTY(QString returnAction    READ returnAction    NOTIFY changed)   ///< what PX4 does there: "RTL", "Land" or "Warn"
    Q_PROPERTY(double  landPercent     READ landPercent     NOTIFY changed)   ///< emergency level (0: unknown)
    Q_PROPERTY(QString landAction      READ landAction      NOTIFY changed)   ///< "Land" or "Warn"
    Q_PROPERTY(int     secondsToReturn READ secondsToReturn NOTIFY changed)   ///< until the critical level; -1: unknown

public:
    explicit SprayBatteryBar(QObject *parent = nullptr);

    bool    valid()           const { return _valid; }
    double  percent()         const { return _percent; }
    double  returnPercent()   const { return _returnPercent; }
    QString returnAction()    const { return _returnAction; }
    double  landPercent()     const { return _landPercent; }
    QString landAction()      const { return _landAction; }
    int     secondsToReturn() const { return _secondsToReturn; }

signals:
    void changed();

private:
    void _update();
    void _readFailsafeLevels(Vehicle *vehicle);
    double _drainPerSecond() const;   ///< % per second, from recent samples; 0: not known yet

    QTimer            _timer;
    QElapsedTimer     _clock;
    QPointer<Vehicle> _vehicle;
    std::deque<std::pair<qint64, double>> _samples;   ///< (ms, %) while armed

    bool    _valid           = false;
    double  _percent         = 0.0;
    double  _returnPercent   = 0.0;
    QString _returnAction;
    double  _landPercent     = 0.0;
    QString _landAction;
    int     _secondsToReturn = -1;
};
