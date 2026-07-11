#ifndef INCLUDE_UDMABUFINPUTWORKER_H
#define INCLUDE_UDMABUFINPUTWORKER_H

#include <atomic>
#include <vector>

#include <QFile>
#include <QThread>
#include <QString>

#include "dsp/dsptypes.h"

class SampleSinkFifo;

class UdmaBufInputWorker : public QThread
{
    Q_OBJECT

public:
    static constexpr quint32 HeaderVersion = 1;
    static constexpr quint32 HeaderBytes = 64;
    static constexpr quint32 PatternTone = 1;
    static constexpr quint32 PatternRamp = 2;

    struct MmapHeader
    {
        char magic[8];
        quint32 version;
        quint32 headerBytes;
        quint32 sampleRate;
        quint32 sampleSizeBits;
        quint64 centerFrequency;
        quint64 sampleCapacity;
        quint64 writeSequence;
        quint32 pattern;
        quint32 reserved;
        quint64 producerDrops;
    };

    struct CI16Sample
    {
        qint16 i;
        qint16 q;
    };

    UdmaBufInputWorker(const QString& fileName, SampleSinkFifo *sampleFifo, QObject *parent = nullptr);
    ~UdmaBufInputWorker() override;

    bool startWork();
    void stopWork();
    bool isRunning() const { return m_running.load(std::memory_order_acquire); }
    quint64 getSamplesCount() const { return m_samplesCount.load(std::memory_order_relaxed); }
    quint64 getContinuityErrors() const { return m_continuityErrors.load(std::memory_order_relaxed); }
    quint64 getFifoDroppedSamples() const { return m_fifoDroppedSamples.load(std::memory_order_relaxed); }
    quint64 getUpstreamDroppedSamples() const { return m_upstreamDroppedSamples.load(std::memory_order_relaxed); }

    static bool readHeader(const QString& fileName, MmapHeader& header, QString& error);
    static bool validateHeader(const MmapHeader& header, quint64 fileSize, QString& error);

protected:
    void run() override;

private:
    static quint64 loadWriteSequence(const MmapHeader *header);
    void drainAvailable(const MmapHeader *header, const CI16Sample *ring);
    void convertAndWrite(const CI16Sample *samples, quint32 count, quint64 firstSequence, quint32 pattern);

    QString m_fileName;
    SampleSinkFifo *m_sampleFifo;
    std::atomic_bool m_running;
    std::atomic<quint64> m_samplesCount;
    std::atomic<quint64> m_continuityErrors;
    std::atomic<quint64> m_fifoDroppedSamples;
    std::atomic<quint64> m_upstreamDroppedSamples;
    quint64 m_consumerSequence;
    std::vector<CI16Sample> m_ci16Buffer;
    SampleVector m_sampleBuffer;
};

static_assert(sizeof(UdmaBufInputWorker::MmapHeader) == UdmaBufInputWorker::HeaderBytes,
    "udmabufinput mmap header layout changed");
static_assert(sizeof(UdmaBufInputWorker::CI16Sample) == 4, "ci16 sample must occupy four bytes");

#endif // INCLUDE_UDMABUFINPUTWORKER_H
