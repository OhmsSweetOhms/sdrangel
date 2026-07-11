#include "udmabufinput.h"

#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTime>

#include "SWGDeviceReport.h"
#include "SWGDeviceSettings.h"
#include "SWGDeviceState.h"
#include "SWGUdmaBufInputReport.h"
#include "SWGUdmaBufInputSettings.h"
#include "device/deviceapi.h"
#include "dsp/dspcommands.h"
#include "dsp/samplesinkfifo.h"
#include "udmabufinputworker.h"

MESSAGE_CLASS_DEFINITION(UdmaBufInput::MsgConfigureUdmaBufInput, Message)
MESSAGE_CLASS_DEFINITION(UdmaBufInput::MsgConfigureFileSourceName, Message)
MESSAGE_CLASS_DEFINITION(UdmaBufInput::MsgConfigureUdmaBufInputWork, Message)
MESSAGE_CLASS_DEFINITION(UdmaBufInput::MsgConfigureUdmaBufInputStreamTiming, Message)
MESSAGE_CLASS_DEFINITION(UdmaBufInput::MsgConfigureFileSourceSeek, Message)
MESSAGE_CLASS_DEFINITION(UdmaBufInput::MsgStartStop, Message)
MESSAGE_CLASS_DEFINITION(UdmaBufInput::MsgPlayPause, Message)
MESSAGE_CLASS_DEFINITION(UdmaBufInput::MsgReportFileSourceAcquisition, Message)
MESSAGE_CLASS_DEFINITION(UdmaBufInput::MsgReportUdmaBufInputStreamData, Message)
MESSAGE_CLASS_DEFINITION(UdmaBufInput::MsgReportUdmaBufInputStreamTiming, Message)
MESSAGE_CLASS_DEFINITION(UdmaBufInput::MsgReportHeaderCRC, Message)

UdmaBufInput::UdmaBufInput(DeviceAPI *deviceAPI) :
    m_deviceAPI(deviceAPI),
    m_settings(),
    m_worker(nullptr),
    m_deviceDescription("UdmaBufInput"),
    m_sampleRate(48000),
    m_centerFrequency(435000000),
    m_headerValid(false),
    m_networkManager(new QNetworkAccessManager(this))
{
    m_sampleFifo.setLabel(m_deviceDescription);
    m_deviceAPI->setNbSourceStreams(1);
    connect(m_networkManager, &QNetworkAccessManager::finished,
        this, &UdmaBufInput::networkManagerFinished);
}

UdmaBufInput::~UdmaBufInput()
{
    stop();
}

void UdmaBufInput::destroy()
{
    delete this;
}

void UdmaBufInput::init()
{
    if (!m_settings.m_fileName.isEmpty()) {
        openMmapFile();
    }
    postSignalNotification();
}

bool UdmaBufInput::openMmapFile()
{
    UdmaBufInputWorker::MmapHeader header;
    QString error;
    m_headerValid = UdmaBufInputWorker::readHeader(m_settings.m_fileName, header, error);

    if (m_headerValid)
    {
        m_sampleRate = static_cast<int>(header.sampleRate);
        m_centerFrequency = header.centerFrequency;
        qInfo() << "UdmaBufInput::openMmapFile:" << m_settings.m_fileName
                 << "sampleRate" << m_sampleRate
                 << "centerFrequency" << m_centerFrequency
                 << "capacity" << header.sampleCapacity
                 << "pattern" << header.pattern;
        postSignalNotification();
    }
    else
    {
        qCritical() << "UdmaBufInput::openMmapFile:" << error;
    }

    reportStreamDataToGUI(m_headerValid);
    return m_headerValid;
}

bool UdmaBufInput::start()
{
    QMutexLocker locker(&m_mutex);
    if (m_worker) {
        return true;
    }
    if (!m_headerValid && !openMmapFile()) {
        return false;
    }
    if (!m_sampleFifo.setSize(SampleSinkFifo::getSizePolicy(m_sampleRate))) {
        qCritical() << "UdmaBufInput::start: could not allocate sample FIFO";
        return false;
    }

    m_worker = new UdmaBufInputWorker(m_settings.m_fileName, &m_sampleFifo);
    if (!m_worker->startWork()) {
        delete m_worker;
        m_worker = nullptr;
        return false;
    }

    postSignalNotification();
    if (getMessageQueueToGUI()) {
        getMessageQueueToGUI()->push(MsgReportFileSourceAcquisition::create(true));
    }
    qInfo() << "UdmaBufInput::start: worker started";
    return true;
}

void UdmaBufInput::stop()
{
    QMutexLocker locker(&m_mutex);
    if (m_worker)
    {
        m_worker->stopWork();
        qInfo() << "UdmaBufInput::stop: samples" << m_worker->getSamplesCount()
                 << "continuityErrors" << m_worker->getContinuityErrors()
                 << "fifoDropped" << m_worker->getFifoDroppedSamples()
                 << "upstreamDropped" << m_worker->getUpstreamDroppedSamples();
        delete m_worker;
        m_worker = nullptr;
    }
    if (getMessageQueueToGUI()) {
        getMessageQueueToGUI()->push(MsgReportFileSourceAcquisition::create(false));
    }
}

void UdmaBufInput::postSignalNotification()
{
    m_deviceAPI->getDeviceEngineInputMessageQueue()->push(
        new DSPSignalNotification(m_sampleRate, m_centerFrequency));
    if (getMessageQueueToGUI()) {
        getMessageQueueToGUI()->push(new DSPSignalNotification(m_sampleRate, m_centerFrequency));
    }
}

void UdmaBufInput::reportStreamDataToGUI(bool headerValid)
{
    if (getMessageQueueToGUI())
    {
        getMessageQueueToGUI()->push(MsgReportHeaderCRC::create(headerValid));
        if (headerValid) {
            getMessageQueueToGUI()->push(
                MsgReportUdmaBufInputStreamData::create(m_sampleRate, m_centerFrequency));
        }
    }
}

QByteArray UdmaBufInput::serialize() const
{
    return m_settings.serialize();
}

bool UdmaBufInput::deserialize(const QByteArray& data)
{
    const bool success = m_settings.deserialize(data);
    if (!success) {
        m_settings.resetToDefaults();
    }
    m_inputMessageQueue.push(MsgConfigureUdmaBufInput::create(m_settings, {}, true));
    return success;
}

bool UdmaBufInput::handleMessage(const Message& message)
{
    if (MsgConfigureUdmaBufInput::match(message))
    {
        const auto& config = static_cast<const MsgConfigureUdmaBufInput&>(message);
        return applySettings(config.getSettings(), config.getSettingsKeys(), config.getForce());
    }
    if (MsgConfigureFileSourceName::match(message))
    {
        const auto& config = static_cast<const MsgConfigureFileSourceName&>(message);
        const bool wasRunning = m_worker != nullptr;
        if (wasRunning) {
            stop();
        }
        m_settings.m_fileName = config.getFileName();
        const bool opened = openMmapFile();
        if (opened && wasRunning) {
            start();
        }
        return true;
    }
    if (MsgConfigureUdmaBufInputWork::match(message))
    {
        const auto& command = static_cast<const MsgConfigureUdmaBufInputWork&>(message);
        if (command.isWorking()) {
            start();
        } else {
            stop();
        }
        return true;
    }
    if (MsgConfigureUdmaBufInputStreamTiming::match(message))
    {
        if (m_worker && getMessageQueueToGUI()) {
            getMessageQueueToGUI()->push(
                MsgReportUdmaBufInputStreamTiming::create(m_worker->getSamplesCount()));
        }
        return true;
    }
    if (MsgConfigureFileSourceSeek::match(message)) {
        return true; // A live ring has no seek operation.
    }
    if (MsgStartStop::match(message))
    {
        const bool run = static_cast<const MsgStartStop&>(message).getStartStop();
        if (run) {
            if (m_deviceAPI->initDeviceEngine()) {
                m_deviceAPI->startDeviceEngine();
            }
        } else {
            m_deviceAPI->stopDeviceEngine();
        }
        if (m_settings.m_useReverseAPI) {
            webapiReverseSendStartStop(run);
        }
        return true;
    }
    return false;
}

bool UdmaBufInput::applySettings(
        const UdmaBufInputSettings& settings,
        const QList<QString>& settingsKeys,
        bool force)
{
    const bool fileChanged = force || settingsKeys.contains("fileName");
    const bool wasRunning = fileChanged && (m_worker != nullptr);
    if (wasRunning) {
        stop();
    }

    if (force) {
        m_settings = settings;
    } else {
        m_settings.applySettings(settingsKeys, settings);
    }

    if (fileChanged) {
        m_headerValid = false;
        if (!m_settings.m_fileName.isEmpty() && openMmapFile() && wasRunning) {
            start();
        }
    }
    if (settings.m_useReverseAPI) {
        webapiReverseSendSettings(settingsKeys, settings, force);
    }
    return true;
}

int UdmaBufInput::webapiSettingsGet(
        SWGSDRangel::SWGDeviceSettings& response,
        QString& errorMessage)
{
    Q_UNUSED(errorMessage)
    response.setUdmaBufInputSettings(new SWGSDRangel::SWGUdmaBufInputSettings());
    response.getUdmaBufInputSettings()->init();
    webapiFormatDeviceSettings(response, m_settings, m_sampleRate, m_centerFrequency);
    return 200;
}

int UdmaBufInput::webapiSettingsPutPatch(
        bool force,
        const QStringList& deviceSettingsKeys,
        SWGSDRangel::SWGDeviceSettings& response,
        QString& errorMessage)
{
    Q_UNUSED(errorMessage)
    UdmaBufInputSettings settings = m_settings;
    webapiUpdateDeviceSettings(settings, deviceSettingsKeys, response);
    m_inputMessageQueue.push(MsgConfigureUdmaBufInput::create(settings, deviceSettingsKeys, force));
    if (getMessageQueueToGUI()) {
        getMessageQueueToGUI()->push(MsgConfigureUdmaBufInput::create(settings, deviceSettingsKeys, force));
    }
    webapiFormatDeviceSettings(response, settings, m_sampleRate, m_centerFrequency);
    return 200;
}

int UdmaBufInput::webapiRunGet(SWGSDRangel::SWGDeviceState& response, QString& errorMessage)
{
    Q_UNUSED(errorMessage)
    m_deviceAPI->getDeviceEngineStateStr(*response.getState());
    return 200;
}

int UdmaBufInput::webapiRun(
        bool run, SWGSDRangel::SWGDeviceState& response, QString& errorMessage)
{
    Q_UNUSED(errorMessage)
    m_deviceAPI->getDeviceEngineStateStr(*response.getState());
    m_inputMessageQueue.push(MsgStartStop::create(run));
    if (getMessageQueueToGUI()) {
        getMessageQueueToGUI()->push(MsgStartStop::create(run));
    }
    return 200;
}

int UdmaBufInput::webapiReportGet(
        SWGSDRangel::SWGDeviceReport& response, QString& errorMessage)
{
    Q_UNUSED(errorMessage)
    response.setUdmaBufInputReport(new SWGSDRangel::SWGUdmaBufInputReport());
    response.getUdmaBufInputReport()->init();
    response.getUdmaBufInputReport()->setFileName(new QString(m_settings.m_fileName));
    response.getUdmaBufInputReport()->setSampleRate(m_sampleRate);
    response.getUdmaBufInputReport()->setCenterFrequency(m_centerFrequency);
    return 200;
}

void UdmaBufInput::webapiFormatDeviceSettings(
        SWGSDRangel::SWGDeviceSettings& response,
        const UdmaBufInputSettings& settings,
        int sampleRate,
        quint64 centerFrequency)
{
    auto *udmaSettings = response.getUdmaBufInputSettings();
    udmaSettings->setFileName(new QString(settings.m_fileName));
    udmaSettings->setSampleRate(sampleRate);
    udmaSettings->setCenterFrequency(centerFrequency);
}

void UdmaBufInput::webapiUpdateDeviceSettings(
        UdmaBufInputSettings& settings,
        const QStringList& keys,
        SWGSDRangel::SWGDeviceSettings& response)
{
    auto *udmaSettings = response.getUdmaBufInputSettings();
    if (keys.contains("fileName") && udmaSettings->getFileName()) {
        settings.m_fileName = *udmaSettings->getFileName();
    }
}

void UdmaBufInput::webapiReverseSendSettings(
        const QList<QString>& keys, const UdmaBufInputSettings& settings, bool force)
{
    Q_UNUSED(keys)
    Q_UNUSED(force)
    auto *deviceSettings = new SWGSDRangel::SWGDeviceSettings();
    deviceSettings->setDirection(0);
    deviceSettings->setOriginatorIndex(m_deviceAPI->getDeviceSetIndex());
    deviceSettings->setDeviceHwType(new QString("UdmaBufInput"));
    deviceSettings->setUdmaBufInputSettings(new SWGSDRangel::SWGUdmaBufInputSettings());
    deviceSettings->getUdmaBufInputSettings()->setFileName(new QString(settings.m_fileName));
    deviceSettings->getUdmaBufInputSettings()->setSampleRate(m_sampleRate);
    deviceSettings->getUdmaBufInputSettings()->setCenterFrequency(m_centerFrequency);

    const QString url = QString("http://%1:%2/sdrangel/deviceset/%3/device/settings")
        .arg(settings.m_reverseAPIAddress).arg(settings.m_reverseAPIPort)
        .arg(settings.m_reverseAPIDeviceIndex);
    m_networkRequest.setUrl(QUrl(url));
    m_networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto *buffer = new QBuffer();
    buffer->open(QIODevice::ReadWrite);
    buffer->write(deviceSettings->asJson().toUtf8());
    buffer->seek(0);
    QNetworkReply *reply = m_networkManager->sendCustomRequest(m_networkRequest, "PATCH", buffer);
    buffer->setParent(reply);
    delete deviceSettings;
}

void UdmaBufInput::webapiReverseSendStartStop(bool start)
{
    const QString url = QString("http://%1:%2/sdrangel/deviceset/%3/device/run")
        .arg(m_settings.m_reverseAPIAddress).arg(m_settings.m_reverseAPIPort)
        .arg(m_settings.m_reverseAPIDeviceIndex);
    m_networkRequest.setUrl(QUrl(url));
    m_networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto *buffer = new QBuffer();
    buffer->open(QIODevice::ReadWrite);
    QNetworkReply *reply = m_networkManager->sendCustomRequest(
        m_networkRequest, start ? "POST" : "DELETE", buffer);
    buffer->setParent(reply);
}

void UdmaBufInput::networkManagerFinished(QNetworkReply *reply)
{
    if (reply->error()) {
        qWarning() << "UdmaBufInput reverse API:" << reply->errorString();
    }
    reply->deleteLater();
}
