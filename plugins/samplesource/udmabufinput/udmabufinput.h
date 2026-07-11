#ifndef INCLUDE_UDMABUFINPUT_H
#define INCLUDE_UDMABUFINPUT_H

#include <QMutex>
#include <QNetworkRequest>
#include <QString>

#include "dsp/devicesamplesource.h"
#include "udmabufinputsettings.h"

class DeviceAPI;
class QNetworkAccessManager;
class QNetworkReply;
class UdmaBufInputWorker;

class UdmaBufInput : public DeviceSampleSource
{
    Q_OBJECT

public:
    class MsgConfigureUdmaBufInput : public Message
    {
        MESSAGE_CLASS_DECLARATION
    public:
        const UdmaBufInputSettings& getSettings() const { return m_settings; }
        const QList<QString>& getSettingsKeys() const { return m_settingsKeys; }
        bool getForce() const { return m_force; }
        static MsgConfigureUdmaBufInput *create(
            const UdmaBufInputSettings& settings, const QList<QString>& settingsKeys, bool force)
        {
            return new MsgConfigureUdmaBufInput(settings, settingsKeys, force);
        }
    private:
        MsgConfigureUdmaBufInput(
            const UdmaBufInputSettings& settings, const QList<QString>& settingsKeys, bool force) :
            m_settings(settings), m_settingsKeys(settingsKeys), m_force(force) {}
        UdmaBufInputSettings m_settings;
        QList<QString> m_settingsKeys;
        bool m_force;
    };

    class MsgConfigureFileSourceName : public Message
    {
        MESSAGE_CLASS_DECLARATION
    public:
        const QString& getFileName() const { return m_fileName; }
        static MsgConfigureFileSourceName *create(const QString& fileName)
        {
            return new MsgConfigureFileSourceName(fileName);
        }
    private:
        explicit MsgConfigureFileSourceName(const QString& fileName) : m_fileName(fileName) {}
        QString m_fileName;
    };

    class MsgConfigureUdmaBufInputWork : public Message
    {
        MESSAGE_CLASS_DECLARATION
    public:
        bool isWorking() const { return m_working; }
        static MsgConfigureUdmaBufInputWork *create(bool working)
        {
            return new MsgConfigureUdmaBufInputWork(working);
        }
    private:
        explicit MsgConfigureUdmaBufInputWork(bool working) : m_working(working) {}
        bool m_working;
    };

    class MsgConfigureUdmaBufInputStreamTiming : public Message
    {
        MESSAGE_CLASS_DECLARATION
    public:
        static MsgConfigureUdmaBufInputStreamTiming *create()
        {
            return new MsgConfigureUdmaBufInputStreamTiming();
        }
    private:
        MsgConfigureUdmaBufInputStreamTiming() = default;
    };

    class MsgConfigureFileSourceSeek : public Message
    {
        MESSAGE_CLASS_DECLARATION
    public:
        static MsgConfigureFileSourceSeek *create(int position)
        {
            return new MsgConfigureFileSourceSeek(position);
        }
    private:
        explicit MsgConfigureFileSourceSeek(int position) { Q_UNUSED(position) }
    };

    class MsgStartStop : public Message
    {
        MESSAGE_CLASS_DECLARATION
    public:
        bool getStartStop() const { return m_startStop; }
        static MsgStartStop *create(bool startStop) { return new MsgStartStop(startStop); }
    private:
        explicit MsgStartStop(bool startStop) : m_startStop(startStop) {}
        bool m_startStop;
    };

    class MsgPlayPause : public Message
    {
        MESSAGE_CLASS_DECLARATION
    public:
        bool getPlayPause() const { return m_playPause; }
        static MsgPlayPause *create(bool playPause) { return new MsgPlayPause(playPause); }
    private:
        explicit MsgPlayPause(bool playPause) : m_playPause(playPause) {}
        bool m_playPause;
    };

    class MsgReportFileSourceAcquisition : public Message
    {
        MESSAGE_CLASS_DECLARATION
    public:
        bool getAcquisition() const { return m_acquisition; }
        static MsgReportFileSourceAcquisition *create(bool acquisition)
        {
            return new MsgReportFileSourceAcquisition(acquisition);
        }
    private:
        explicit MsgReportFileSourceAcquisition(bool acquisition) : m_acquisition(acquisition) {}
        bool m_acquisition;
    };

    class MsgReportUdmaBufInputStreamData : public Message
    {
        MESSAGE_CLASS_DECLARATION
    public:
        int getSampleRate() const { return m_sampleRate; }
        quint32 getSampleSize() const { return 16; }
        quint64 getCenterFrequency() const { return m_centerFrequency; }
        quint64 getStartingTimeStamp() const { return 0; }
        quint64 getRecordLengthMuSec() const { return 0; }
        static MsgReportUdmaBufInputStreamData *create(int sampleRate, quint64 centerFrequency)
        {
            return new MsgReportUdmaBufInputStreamData(sampleRate, centerFrequency);
        }
    private:
        MsgReportUdmaBufInputStreamData(int sampleRate, quint64 centerFrequency) :
            m_sampleRate(sampleRate), m_centerFrequency(centerFrequency) {}
        int m_sampleRate;
        quint64 m_centerFrequency;
    };

    class MsgReportUdmaBufInputStreamTiming : public Message
    {
        MESSAGE_CLASS_DECLARATION
    public:
        quint64 getSamplesCount() const { return m_samplesCount; }
        static MsgReportUdmaBufInputStreamTiming *create(quint64 samplesCount)
        {
            return new MsgReportUdmaBufInputStreamTiming(samplesCount);
        }
    private:
        explicit MsgReportUdmaBufInputStreamTiming(quint64 samplesCount) : m_samplesCount(samplesCount) {}
        quint64 m_samplesCount;
    };

    class MsgReportHeaderCRC : public Message
    {
        MESSAGE_CLASS_DECLARATION
    public:
        bool isOK() const { return m_ok; }
        static MsgReportHeaderCRC *create(bool ok) { return new MsgReportHeaderCRC(ok); }
    private:
        explicit MsgReportHeaderCRC(bool ok) : m_ok(ok) {}
        bool m_ok;
    };

    explicit UdmaBufInput(DeviceAPI *deviceAPI);
    ~UdmaBufInput() override;
    void destroy() override;
    void init() override;
    bool start() override;
    void stop() override;
    QByteArray serialize() const override;
    bool deserialize(const QByteArray& data) override;
    void setMessageQueueToGUI(MessageQueue *queue) override { m_guiMessageQueue = queue; }
    const QString& getDeviceDescription() const override { return m_deviceDescription; }
    int getSampleRate() const override { return m_sampleRate; }
    void setSampleRate(int sampleRate) override { Q_UNUSED(sampleRate) }
    quint64 getCenterFrequency() const override { return m_centerFrequency; }
    void setCenterFrequency(qint64 centerFrequency) override { Q_UNUSED(centerFrequency) }
    bool handleMessage(const Message& message) override;

    int webapiSettingsGet(SWGSDRangel::SWGDeviceSettings& response, QString& errorMessage) override;
    int webapiSettingsPutPatch(bool force, const QStringList& deviceSettingsKeys,
        SWGSDRangel::SWGDeviceSettings& response, QString& errorMessage) override;
    int webapiRunGet(SWGSDRangel::SWGDeviceState& response, QString& errorMessage) override;
    int webapiRun(bool run, SWGSDRangel::SWGDeviceState& response, QString& errorMessage) override;
    int webapiReportGet(SWGSDRangel::SWGDeviceReport& response, QString& errorMessage) override;

    static void webapiFormatDeviceSettings(
        SWGSDRangel::SWGDeviceSettings& response,
        const UdmaBufInputSettings& settings,
        int sampleRate = 0,
        quint64 centerFrequency = 0);
    static void webapiUpdateDeviceSettings(UdmaBufInputSettings& settings,
        const QStringList& deviceSettingsKeys, SWGSDRangel::SWGDeviceSettings& response);

private slots:
    void networkManagerFinished(QNetworkReply *reply);

private:
    bool openMmapFile();
    bool applySettings(const UdmaBufInputSettings& settings, const QList<QString>& settingsKeys, bool force);
    void postSignalNotification();
    void reportStreamDataToGUI(bool headerValid);
    void webapiReverseSendSettings(
        const QList<QString>& deviceSettingsKeys, const UdmaBufInputSettings& settings, bool force);
    void webapiReverseSendStartStop(bool start);

    DeviceAPI *m_deviceAPI;
    QMutex m_mutex;
    UdmaBufInputSettings m_settings;
    UdmaBufInputWorker *m_worker;
    QString m_deviceDescription;
    int m_sampleRate;
    quint64 m_centerFrequency;
    bool m_headerValid;
    QNetworkAccessManager *m_networkManager;
    QNetworkRequest m_networkRequest;
};

#endif // INCLUDE_UDMABUFINPUT_H
