#include "udmabufinputworker.h"

#include <algorithm>
#include <cstring>

#include <QDebug>
#include <QElapsedTimer>

#include "dsp/samplesinkfifo.h"

namespace {
constexpr char MmapMagic[8] = {'U', 'D', 'M', 'A', 'I', 'Q', '1', '\0'};
constexpr quint32 MaxDrainSamples = 16384;
constexpr unsigned long IdleSleepUsec = 200;
// q-01 v2 seqlock read bound (design-ring-fanout-spec.md Sec 5 v2 item 4): a
// live publish window is ns-wide, so a clean even-seq read lands in the first
// few spins; this cap only matters when the writer was SIGKILLed between the
// odd and even seq bumps (seq stranded odd), in which case loadWriteSequence
// gives up and the caller routes to the existing "producer gone" idle path
// instead of hard-spinning the engine thread.
constexpr unsigned SeqlockMaxSpins = 1024;
// plan-06 Finding-1: low-rate SampleSinkFifo occupancy telemetry. Cheap
// (fill()/size() are mutex-guarded O(1) reads) and rate-limited so it costs
// nothing on the hot drain path; gives operators/diagnostics a live signal
// to distinguish "bounded occupancy" (healthy) from "slow growth toward the
// ring's fixed capacity" (the steady-state rate-deficit failure mode) without
// needing to re-instrument for every future capacity investigation.
constexpr qint64 FifoLogIntervalMs = 5000;

FixReal expandCI16(qint16 value)
{
    const quint32 shifted = static_cast<quint32>(static_cast<qint32>(value)) << 8;
    FixReal result;
    static_assert(sizeof(result) == sizeof(shifted), "24-bit samples require a 32-bit FixReal");
    std::memcpy(&result, &shifted, sizeof(result));
    return result;
}
}

UdmaBufInputWorker::UdmaBufInputWorker(
        const QString& fileName,
        SampleSinkFifo *sampleFifo,
        QObject *parent) :
    QThread(parent),
    m_fileName(fileName),
    m_sampleFifo(sampleFifo),
    m_running(false),
    m_samplesCount(0),
    m_continuityErrors(0),
    m_fifoDroppedSamples(0),
    m_upstreamDroppedSamples(0),
    m_consumerSequence(0),
    m_ci16Buffer(MaxDrainSamples),
    m_sampleBuffer(MaxDrainSamples)
{
}

UdmaBufInputWorker::~UdmaBufInputWorker()
{
    stopWork();
}

bool UdmaBufInputWorker::startWork()
{
    if (isRunning()) {
        return true;
    }

    MmapHeader header;
    QString error;
    if (!readHeader(m_fileName, header, error)) {
        qCritical() << "UdmaBufInputWorker::startWork:" << error;
        return false;
    }

    m_running.store(true, std::memory_order_release);
    start();
    return true;
}

void UdmaBufInputWorker::stopWork()
{
    if (isRunning()) {
        m_running.store(false, std::memory_order_release);
        wait();
    }
}

bool UdmaBufInputWorker::readHeader(const QString& fileName, MmapHeader& header, QString& error)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QString("cannot open mmap file %1: %2").arg(fileName, file.errorString());
        return false;
    }

    if (file.read(reinterpret_cast<char *>(&header), sizeof(header)) != sizeof(header)) {
        error = QString("mmap file %1 is shorter than the %2-byte header").arg(fileName).arg(HeaderBytes);
        return false;
    }

    return validateHeader(header, file.size(), error);
}

bool UdmaBufInputWorker::validateHeader(const MmapHeader& header, quint64 fileSize, QString& error)
{
    if (std::memcmp(header.magic, MmapMagic, sizeof(MmapMagic)) != 0) {
        error = "mmap header magic is not UDMAIQ1";
        return false;
    }
    if (header.version != HeaderVersion || header.headerBytes != HeaderBytes) {
        error = QString("unsupported mmap header version/layout %1/%2 (need v%3, a v1 bundle is stale)")
            .arg(header.version).arg(header.headerBytes).arg(HeaderVersion);
        return false;
    }
    // q-01 v2 (spec Sec 5 v2 item 2): `version==2` is stored LAST at init and
    // an even `seq` means no publish is in flight -- both are required before
    // first use so a reader can never latch a half-initialized header.
    if ((header.seq & 1u) != 0u) {
        error = QString("mmap header seqlock word is odd (%1): producer mid-publish or not yet initialized")
            .arg(header.seq);
        return false;
    }
    if (header.sampleRate == 0 || header.sampleSizeBits != 16 || header.sampleCapacity == 0) {
        error = QString("invalid rate/sample-size/capacity %1/%2/%3")
            .arg(header.sampleRate).arg(header.sampleSizeBits).arg(header.sampleCapacity);
        return false;
    }

    const quint64 requiredBytes = HeaderBytes + header.sampleCapacity * sizeof(CI16Sample);
    if (requiredBytes > fileSize) {
        error = QString("mmap file is %1 bytes but header requires %2").arg(fileSize).arg(requiredBytes);
        return false;
    }

    return true;
}

bool UdmaBufInputWorker::loadWriteSequence(const MmapHeader *header, quint64& value)
{
    // q-01 v2 bounded seqlock read (design-ring-fanout-spec.md Sec 5 v2 item
    // 4): the ARMHF writer publishes writeSequence as two 32-bit halves guarded
    // by `seq` (odd = publish in flight). Read seq (s0), acquire-fence, read
    // lo/hi through volatile pointers, acquire-fence, read seq (s1); a read is
    // clean iff s0 is even and s0==s1. This is an inline mirror of
    // capture_ring_read64() -- the fork must NOT include capture_ring.h or any
    // socks header (contract confinement, spec Sec 1). Bounded, unlike
    // capture_ring_read64()'s unbounded spin: on exhaustion (writer SIGKILLed
    // mid-publish, seq stranded odd) return false so the caller idles; the
    // engine's own while(isRunning()) loop supplies the retry.
    const volatile quint32 *seqp = reinterpret_cast<const volatile quint32 *>(&header->seq);
    const volatile quint32 *lop  = reinterpret_cast<const volatile quint32 *>(&header->writeSequenceLo);
    const volatile quint32 *hip  = reinterpret_cast<const volatile quint32 *>(&header->writeSequenceHi);

    for (unsigned spin = 0; spin < SeqlockMaxSpins; ++spin) {
        const quint32 s0 = *seqp;
        std::atomic_thread_fence(std::memory_order_acquire);
        const quint32 lo = *lop;
        const quint32 hi = *hip;
        std::atomic_thread_fence(std::memory_order_acquire);
        const quint32 s1 = *seqp;
        if ((s0 & 1u) == 0u && s0 == s1) {
            value = (static_cast<quint64>(hi) << 32) | static_cast<quint64>(lo);
            return true;
        }
    }
    return false;
}

quint32 UdmaBufInputWorker::loadVersion(const MmapHeader *header)
{
    // A1 (plan-10): the producer re-inits its header IN PLACE on restart (no
    // O_TRUNC), storing version=0 first and version=HeaderVersion last. Read
    // version through a volatile pointer so the poll in run() re-samples the
    // mapping every iteration instead of the compiler hoisting a stale load.
    return *reinterpret_cast<const volatile quint32 *>(&header->version);
}

void UdmaBufInputWorker::run()
{
    QFile file(m_fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        qCritical() << "UdmaBufInputWorker::run: cannot open" << m_fileName << file.errorString();
        m_running.store(false, std::memory_order_release);
        return;
    }

    uchar *mapping = file.map(0, file.size());
    if (!mapping) {
        qCritical() << "UdmaBufInputWorker::run: cannot map" << m_fileName << file.errorString();
        m_running.store(false, std::memory_order_release);
        return;
    }

    const auto *header = reinterpret_cast<const MmapHeader *>(mapping);
    const auto *ring = reinterpret_cast<const CI16Sample *>(mapping + HeaderBytes);
    quint64 producerSequence = 0;
    // A clean read is guaranteed here (validateHeader already required even
    // seq); if the writer is mid-publish anyway, start from 0 and catch up.
    (void)loadWriteSequence(header, producerSequence);
    m_consumerSequence = producerSequence > header->sampleCapacity
        ? producerSequence - header->sampleCapacity : 0;
    // A1 (plan-10): the producer re-inits the header in place on restart (no
    // O_TRUNC); `version` transitions HeaderVersion -> 0 (invalidate) ->
    // HeaderVersion (re-init complete). Track it so run() can re-validate and
    // resync when a restart is observed via the version dip.
    quint32 lastVersion = loadVersion(header);

    QElapsedTimer fifoLogTimer;
    fifoLogTimer.start();

    while (isRunning())
    {
        // A1 (plan-10): re-validate on any version change. While version !=
        // HeaderVersion the producer is mid-reinit -- idle until it settles.
        // Once it re-reads HeaderVersion, re-run validateHeader (rate/capacity/
        // seqlock/size may have changed with a fresh bundle; a bad one fails
        // loudly and stops the worker rather than reading garbage) and resync
        // m_consumerSequence to the fresh producer's published sequence.
        const quint32 currentVersion = loadVersion(header);
        if (currentVersion != lastVersion)
        {
            if (currentVersion != HeaderVersion)
            {
                QThread::usleep(IdleSleepUsec);
                continue;
            }
            QString error;
            if (!validateHeader(*header, file.size(), error))
            {
                qCritical() << "UdmaBufInputWorker::run: header re-validate failed after producer restart:" << error;
                m_running.store(false, std::memory_order_release);
                break;
            }
            quint64 restartSequence = 0;
            (void)loadWriteSequence(header, restartSequence);
            m_consumerSequence = restartSequence > header->sampleCapacity
                ? restartSequence - header->sampleCapacity : 0;
            lastVersion = currentVersion;
            qInfo() << "UdmaBufInputWorker::run: producer restart re-validated, resync consumerSequence"
                    << m_consumerSequence;
            continue;
        }

        const quint64 before = m_samplesCount.load(std::memory_order_relaxed);
        drainAvailable(header, ring);
        if (m_samplesCount.load(std::memory_order_relaxed) == before) {
            QThread::usleep(IdleSleepUsec);
        }

        if (fifoLogTimer.elapsed() >= FifoLogIntervalMs)
        {
            const unsigned int fifoSize = m_sampleFifo->size();
            const unsigned int fifoFill = m_sampleFifo->fill();
            const double fifoPct = fifoSize > 0 ? (100.0 * fifoFill / fifoSize) : 0.0;
            qInfo().nospace() << "UdmaBufInputWorker::run: fifo occupancy "
                << fifoFill << "/" << fifoSize << " (" << fifoPct << "%)"
                << " samples=" << getSamplesCount()
                << " fifoDropped=" << getFifoDroppedSamples()
                << " upstreamDropped=" << getUpstreamDroppedSamples();
            fifoLogTimer.restart();
        }
    }

    file.unmap(mapping);
    qInfo() << "UdmaBufInputWorker::run: stopped samples" << getSamplesCount()
             << "continuityErrors" << getContinuityErrors()
             << "fifoDropped" << getFifoDroppedSamples()
             << "upstreamDropped" << getUpstreamDroppedSamples();
}

void UdmaBufInputWorker::drainAvailable(const MmapHeader *header, const CI16Sample *ring)
{
    quint64 producerSequence = 0;
    // q-01 v2: a bounded seqlock read failure means the writer is mid-publish
    // (or was killed with seq stranded odd). Skip this drain pass; run()'s
    // no-progress branch then idles IdleSleepUsec and the next iteration
    // retries -- the "producer gone" degrade path (spec Sec 5 v2 item 4).
    if (!loadWriteSequence(header, producerSequence)) {
        return;
    }

    // A1 fix (plan-10): a producer restart republishes writeSequence from 0, so
    // producerSequence can regress BELOW our accumulated m_consumerSequence.
    // Computed naively, producerSequence - m_consumerSequence underflows to a
    // near-2^64 "available" -- mis-accounted as billions of upstream drops and a
    // forced full-ring re-drain. Detect the regression and resync
    // m_consumerSequence with the same formula startup uses (leave the fresh
    // producer at most one ring behind), reporting no spurious drop; the next
    // pass then drains normally. (The version-change re-validate in run() is the
    // companion guard for the case where we also observe the version dip.)
    if (producerSequence < m_consumerSequence) {
        m_consumerSequence = producerSequence > header->sampleCapacity
            ? producerSequence - header->sampleCapacity : 0;
        return;
    }

    quint64 available = producerSequence - m_consumerSequence;

    if (available > header->sampleCapacity)
    {
        const quint64 dropped = available - header->sampleCapacity;
        m_upstreamDroppedSamples.fetch_add(dropped, std::memory_order_relaxed);
        m_consumerSequence = producerSequence - header->sampleCapacity;
        available = header->sampleCapacity;
    }

    while (available > 0 && isRunning())
    {
        const quint32 count = static_cast<quint32>(std::min<quint64>(available, MaxDrainSamples));
        const quint64 offset = m_consumerSequence % header->sampleCapacity;
        const quint32 firstCount = static_cast<quint32>(
            std::min<quint64>(count, header->sampleCapacity - offset));
        std::copy(ring + offset, ring + offset + firstCount, m_ci16Buffer.begin());
        if (firstCount < count) {
            std::copy(ring, ring + (count - firstCount), m_ci16Buffer.begin() + firstCount);
        }

        convertAndWrite(m_ci16Buffer.data(), count, m_consumerSequence, header->pattern);
        m_consumerSequence += count;
        available -= count;
    }
}

void UdmaBufInputWorker::convertAndWrite(
        const CI16Sample *samples,
        quint32 count,
        quint64 firstSequence,
        quint32 pattern)
{
    for (quint32 index = 0; index < count; ++index)
    {
        if (pattern == PatternRamp)
        {
            const quint16 expected = static_cast<quint16>(firstSequence + index);
            if (static_cast<quint16>(samples[index].i) != expected) {
                m_continuityErrors.fetch_add(1, std::memory_order_relaxed);
            }
        }

#if SDR_RX_SAMP_SZ == 24
        m_sampleBuffer[index].setReal(expandCI16(samples[index].i));
        m_sampleBuffer[index].setImag(expandCI16(samples[index].q));
#else
        m_sampleBuffer[index].setReal(samples[index].i);
        m_sampleBuffer[index].setImag(samples[index].q);
#endif
    }

    const unsigned int written = m_sampleFifo->write(
        m_sampleBuffer.cbegin(), m_sampleBuffer.cbegin() + count);
    if (written < count) {
        m_fifoDroppedSamples.fetch_add(count - written, std::memory_order_relaxed);
    }
    m_samplesCount.fetch_add(written, std::memory_order_relaxed);
}
