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
    static constexpr quint32 HeaderVersion = 2;   // q-01: split-word seqlock publish (v2)
    static constexpr quint32 HeaderBytes = 64;
    static constexpr quint32 PatternTone = 1;
    static constexpr quint32 PatternRamp = 2;

    // v2 header (design-ring-fanout-spec.md Sec 5 v2 amendment, the q-01
    // contract change): the two live u64 fields are published by the ARMHF
    // writer as naturally-aligned 32-bit lo/hi pairs guarded by the seqlock
    // word `seq` (former `reserved` @52, odd = publish in flight). Every shared
    // field is a single-copy-atomic 32-bit word on both ABIs -- no 8-byte
    // store the writer's process-private libatomic lock could tear. Offsets are
    // unchanged from v1; the 64-byte size is static-asserted below.
    struct MmapHeader
    {
        char magic[8];              // 0
        quint32 version;            // 8  = 2, written LAST at init
        quint32 headerBytes;        // 12
        quint32 sampleRate;         // 16
        quint32 sampleSizeBits;     // 20
        quint64 centerFrequency;    // 24
        quint64 sampleCapacity;     // 32
        quint32 writeSequenceLo;    // 40
        quint32 writeSequenceHi;    // 44
        quint32 pattern;            // 48
        quint32 seq;                // 52 seqlock word (odd = publish in flight)
        quint32 producerDropsLo;    // 56
        quint32 producerDropsHi;    // 60
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
    static bool loadWriteSequence(const MmapHeader *header, quint64& value);
    static quint32 loadVersion(const MmapHeader *header);
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
