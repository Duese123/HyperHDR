#include <leddevice/LedDevice.h>

//QT include
#include <QResource>
#include <QStringList>
#include <QDir>
#include <QDateTime>
#include <QEventLoop>
#include <QTimer>
#include <QDateTime>

#include <hyperhdrbase/HyperHdrInstance.h>
#include <utils/JsonUtils.h>

//std includes
#include <sstream>
#include <iomanip>

LedDevice::LedDevice(const QJsonObject& deviceConfig, QObject* parent)
	: QObject(parent)
	  , _devConfig(deviceConfig)
	  , _log(Logger::getInstance("LEDDEVICE_"+ deviceConfig["type"].toString("unknown").toLower()))
	  , _ledBuffer(0)
	  , _refreshTimer(nullptr)
	  , _refreshTimerInterval_ms(0)
	  , _latchTime_ms(0)
	  , _ledCount(0)
	  , _isRestoreOrigState(false)
	  , _isEnabled(false)
	  , _isDeviceInitialised(false)
	  , _isDeviceReady(false)
	  , _isOn(false)
	  , _isDeviceInError(false)
	  , _isInSwitchOff (false)
	  , _isBlackScreen (false)
	  , _lastWriteTime(QDateTime::currentDateTime())
	  , _isRefreshEnabled (false)
	  , _semaphore(1)
	  , _consumed(true)
	  , _frames(0)
	  , _incomingframes(0)
	  , _skippedFrames(0)
	  , _framesBegin(QDateTime::currentMSecsSinceEpoch())
{
	_activeDeviceType = deviceConfig["type"].toString("UNSPECIFIED").toLower();

	connect(this, &LedDevice::manualUpdate, this, &LedDevice::rewriteLEDs);
}

LedDevice::~LedDevice()
{
	delete _refreshTimer;
}

void LedDevice::start()
{
	Info(_log, "Start LedDevice '%s'.", QSTRING_CSTR(_activeDeviceType));

	_consumed = true;

	// setup refreshTimer
	if ( _refreshTimer == nullptr )
	{
		_refreshTimer = new QTimer(this);
		_refreshTimer->setTimerType(Qt::PreciseTimer);
		_refreshTimer->setInterval( _refreshTimerInterval_ms );
		connect(_refreshTimer, &QTimer::timeout, this, &LedDevice::rewriteLEDs );
	}

	close();

	_isDeviceInitialised = false;
	// General initialisation and configuration of LedDevice
	if ( init(_devConfig) )
	{
		// Everything is OK -> enable device
		_isDeviceInitialised = true;
		this->enable();
	}
}

void LedDevice::stop()
{
	this->disable();
	this->stopRefreshTimer();
	Info(_log, " Stopped LedDevice '%s'", QSTRING_CSTR(_activeDeviceType) );
}

int LedDevice::open()
{
	_consumed = true;

	_isDeviceReady = true;
	int retval = 0;

	return retval;
}

int LedDevice::close()
{
	_isDeviceReady = false;
	int retval = 0;

	return retval;
}

void LedDevice::setInError(const QString& errorMsg)
{
	_isDeviceInError = true;
	_isDeviceReady = false;
	_isEnabled = false;
	this->stopRefreshTimer();

	Error(_log, "Device disabled, device '%s' signals error: '%s'", QSTRING_CSTR(_activeDeviceType), QSTRING_CSTR(errorMsg));
	emit enableStateChanged(_isEnabled);
}

void LedDevice::enable()
{
	if ( !_isEnabled )
	{
		_consumed = true;
		_isDeviceInError = false;

		if ( ! _isDeviceReady )
		{
			open();
		}

		if ( _isDeviceReady )
		{
			_isEnabled = true;
			if ( switchOn() )
			{
				emit enableStateChanged(_isEnabled);
			}
		}

		this->startRefreshTimer();
	}
}

void LedDevice::disable()
{
	if ( _isEnabled )
	{
		_isEnabled = false;
		this->stopRefreshTimer();

		switchOff();
		close();

		emit enableStateChanged(_isEnabled);
	}
}

void LedDevice::setActiveDeviceType(const QString& deviceType)
{
	_activeDeviceType = deviceType;
}

bool LedDevice::init(const QJsonObject &deviceConfig)
{
	Debug(_log, "deviceConfig: [%s]", QString(QJsonDocument(_devConfig).toJson(QJsonDocument::Compact)).toUtf8().constData() );

	_colorOrder = deviceConfig["colorOrder"].toString("RGB");

	setLedCount( deviceConfig["currentLedCount"].toInt(1) ); // property injected to reflect real led count
	setLatchTime( deviceConfig["latchTime"].toInt( _latchTime_ms ) );
	setRewriteTime ( deviceConfig["rewriteTime"].toInt( _refreshTimerInterval_ms) );

	return true;
}

void LedDevice::startRefreshTimer()
{
	if ( _isDeviceReady && _isEnabled && _refreshTimerInterval_ms > 0)
	{
		Debug(_log, "Starting timer with interval = %ims", _refreshTimer->interval());
		_refreshTimer->start();
	}
}

void LedDevice::stopRefreshTimer()
{
	if ( _refreshTimer != nullptr )
	{
		Debug(_log, "Stopping timer");
		_refreshTimer->stop();
	}
}

int LedDevice::updateLeds(const std::vector<ColorRgb>& ledValues)
{
	qint64 currentTime = QDateTime::currentMSecsSinceEpoch();

	if (currentTime - _framesBegin >= 1000 * 60)
	{

		Info(_log, "LED refresh rate %.2f Hz (total written frames: %i, incoming: %i, skipped: %i, interval: %.2fs). %s",
			_frames / 60.0, _frames, _incomingframes, _skippedFrames, int(currentTime - _framesBegin) / 1000.0,
			(_refreshTimer->isActive())?"Buffer timer is active because you set rewrite time.":"Buffer timer is disabled (rewrite time = 0). Direct writes.");

		_frames = 0;
		_incomingframes = 0;
		_skippedFrames = 0;
		_framesBegin = currentTime;
	}

	_incomingframes++;

	if (!_isEnabled || (!_isOn && !_isBlackScreen) || !_isDeviceReady || _isDeviceInError)
	{
		return -1;
	}
	else
	{
		bool skipUpdate = false;

		_semaphore.acquire();
		if (_consumed)
		{
			_consumed = false;
		}
		else
		{
			skipUpdate = true;
		}
		_lastLedValues = ledValues;
		_semaphore.release();

		if (!_refreshTimer->isActive())
		{
			if (!skipUpdate)
			{
				emit manualUpdate();
			}
			else
				_skippedFrames++;
		}
	}
	
	return 0;
}

int LedDevice::rewriteLEDs()
{
	int retval = -1;

	if ( _isDeviceReady && _isEnabled)
	{		
		qint64 elapsedTimeMs = _lastWriteTime.msecsTo(QDateTime::currentDateTime());
		if (_latchTime_ms == 0 || elapsedTimeMs >= _latchTime_ms)
		{
			_semaphore.acquire();
			std::vector<ColorRgb> copy = _lastLedValues;
			_consumed = true;
			_semaphore.release();

			if (copy.size()>0 && !(!_isEnabled || (!_isOn && !_isBlackScreen) || !_isDeviceReady || _isDeviceInError))
				retval = write(copy);

			_lastWriteTime = QDateTime::currentDateTime();

			_frames++;			
		}		
	}
	else
	{
		// If Device is not ready stop timer
		this->stopRefreshTimer();
	}
	return retval;
}

int LedDevice::writeBlack(int numberOfBlack)
{
	int rc = -1;

	for (int i = 0; i < numberOfBlack; i++)
	{
		if ( _latchTime_ms > 0 )
		{
			// Wait latch time before writing black
			QEventLoop loop;
			QTimer::singleShot(_latchTime_ms, &loop, &QEventLoop::quit);
			loop.exec();
		}

		_semaphore.acquire();
		std::vector<ColorRgb> copy = std::vector<ColorRgb>(static_cast<unsigned long>(_ledCount), ColorRgb::BLACK );
		_lastLedValues = copy;
		_semaphore.release();

		rc = write(copy);
	}
	return rc;
}

bool LedDevice::switchOn()
{
	bool rc = false;

	if ( _isOn )
	{
		rc = true;
	}
	else
	{
		if ( _isEnabled &&_isDeviceInitialised )
		{
			storeState();

			if ( powerOn() )
			{
				_isOn = true;
				rc = true;
			}
		}
	}
	return rc;
}

bool LedDevice::switchOff()
{
	bool rc = false;

	if ( !_isOn && !(_isRestoreOrigState && _isBlackScreen) ) 
	{
		rc = true;
	}
	else
	{
		if ( _isDeviceInitialised )
		{
			// Disable device to ensure no standard Led updates are written/processed
			_isOn = false;
			_isInSwitchOff = true;

			rc = true;

			if ( _isDeviceReady )
			{
				if ( _isRestoreOrigState )
				{
					//Restore devices state
					restoreState();
				}
				else
				{
					powerOff();
				}
			}
		}
	}
	return rc;
}

bool LedDevice::powerOff()
{
	bool rc = false;

	// Simulate power-off by writing a final "Black" to have a defined outcome
	if ( writeBlack() >= 0 )
	{
		rc = true;
	}
	return rc;
}

bool LedDevice::powerOn()
{
	bool rc = true;
	return rc;
}

bool LedDevice::storeState()
{
	bool rc = true;

	if ( _isRestoreOrigState )
	{
		// Save device's original state
		// _originalStateValues = get device's state;
		// store original power on/off state, if available
	}
	return rc;
}

bool LedDevice::restoreState()
{
	bool rc = true;

	if ( _isRestoreOrigState )
	{
		// Restore device's original state
		// update device using _originalStateValues
		// update original power on/off state, if supported
	}
	return rc;
}

QJsonObject LedDevice::discover(const QJsonObject& /*params*/)
{
	QJsonObject devicesDiscovered;

	devicesDiscovered.insert("ledDeviceType", _activeDeviceType);

	QJsonArray deviceList;
	devicesDiscovered.insert("devices", deviceList);

	Debug(_log, "devicesDiscovered: [%s]", QString(QJsonDocument(devicesDiscovered).toJson(QJsonDocument::Compact)).toUtf8().constData() );
	return devicesDiscovered;
}

QString LedDevice::discoverFirst()
{
	QString deviceDiscovered;

	Debug(_log, "deviceDiscovered: [%s]", QSTRING_CSTR(deviceDiscovered) );
	return deviceDiscovered;
}


QJsonObject LedDevice::getProperties(const QJsonObject& params)
{
	Debug(_log, "params: [%s]", QString(QJsonDocument(params).toJson(QJsonDocument::Compact)).toUtf8().constData() );

	QJsonObject properties;

	QJsonObject deviceProperties;
	properties.insert("properties", deviceProperties);

	Debug(_log, "properties: [%s]", QString(QJsonDocument(properties).toJson(QJsonDocument::Compact)).toUtf8().constData() );

	return properties;
}

void LedDevice::setLedCount(int ledCount)
{
	assert(ledCount >= 0);
	_ledCount     = ledCount;
	_ledRGBCount  = _ledCount * sizeof(ColorRgb);
	_ledRGBWCount = _ledCount * sizeof(ColorRgbw);
}

void LedDevice::setLatchTime( int latchTime_ms )
{
	assert(latchTime_ms >= 0);
	_latchTime_ms = latchTime_ms;
	Debug(_log, "LatchTime updated to %dms", _latchTime_ms);
}

void LedDevice::setRewriteTime( int rewriteTime_ms )
{
	assert(rewriteTime_ms >= 0);
	_refreshTimerInterval_ms = rewriteTime_ms;

	if (_refreshTimerInterval_ms > 0)
	{

		_isRefreshEnabled = true;

		if (_refreshTimerInterval_ms <= _latchTime_ms)
		{
			int new_refresh_timer_interval = _latchTime_ms + 10;
			Warning(_log, "latchTime(%d) is bigger/equal rewriteTime(%d), set rewriteTime to %dms", _latchTime_ms, _refreshTimerInterval_ms, new_refresh_timer_interval);
			_refreshTimerInterval_ms = new_refresh_timer_interval;
			_refreshTimer->setInterval(_refreshTimerInterval_ms);
		}

		Debug(_log, "Refresh interval = %dms", _refreshTimerInterval_ms);
		_refreshTimer->setInterval(_refreshTimerInterval_ms);

		_lastWriteTime = QDateTime::currentDateTime();

		startRefreshTimer();
	}
	else
		stopRefreshTimer();

	Debug(_log, "RewriteTime updated to %dms", _refreshTimerInterval_ms);
}

void LedDevice::printLedValues(const std::vector<ColorRgb>& ledValues)
{
	std::cout << "LedValues [" << ledValues.size() <<"] [";
	for (const ColorRgb& color : ledValues)
	{
		std::cout << color;
	}
	std::cout << "]" << std::endl;
}

QString LedDevice::uint8_t_to_hex_string(const uint8_t * data, const int size, int number) const
{
	if ( number <= 0 || number > size)
	{
		number = size;
	}

	QByteArray bytes (reinterpret_cast<const char*>(data), number);
	#if (QT_VERSION >= QT_VERSION_CHECK(5, 9, 0))
		return bytes.toHex(':');
	#else
		return bytes.toHex();
	#endif
}

QString LedDevice::toHex(const QByteArray& data, int number) const
{
	if ( number <= 0 || number > data.size())
	{
		number = data.size();
	}

#if (QT_VERSION >= QT_VERSION_CHECK(5, 9, 0))
	return data.left(number).toHex(':');
#else
	return data.left(number).toHex();
#endif
}
