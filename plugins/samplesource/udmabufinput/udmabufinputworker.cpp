#include "udmabufinputworker.h"

#include <algorithm>
#include <cstring>

#include <QDebug>

#include "dsp/samplesinkfifo.h"

namespace {
constexpr char MmapMagic[8] = {'U', 'D', 'M', 'A', 'I', 'Q', '1', '\0'};
constexpr quint32 MaxDrainSamples = 16384;
constexpr unsigned long IdleSleepUsec = 200;

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
        error = QString("unsupported mmap header version/layout %1/%2").arg(header.version).arg(header.headerBytes);
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

quint64 UdmaBufInputWorker::loadWriteSequence(const MmapHeader *header)
{
    return __atomic_load_n(&header->writeSequence, __ATOMIC_ACQUIRE);
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
    const quint64 producerSequence = loadWriteSequence(header);
    m_consumerSequence = producerSequence > header->sampleCapacity
        ? producerSequence - header->sampleCapacity : 0;

    while (isRunning())
    {
        const quint64 before = m_samplesCount.load(std::memory_order_relaxed);
        drainAvailable(header, ring);
        if (m_samplesCount.load(std::memory_order_relaxed) == before) {
            QThread::usleep(IdleSleepUsec);
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
    const quint64 producerSequence = loadWriteSequence(header);
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
