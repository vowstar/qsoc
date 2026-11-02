// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIGRAPHICS_H
#define QTUIGRAPHICS_H

#include <QByteArray>
#include <QImage>
#include <QProcessEnvironment>
#include <QSet>
#include <QSize>
#include <QString>

#include <cstdint>

/**
 * @brief What this terminal can show, and the graphics it currently holds.
 * @details One terminal serves the whole client, so the capability and the
 *          terminal's image store are process-wide. The protocol is derived
 *          from the environment on each call and from one startup probe.
 */
namespace QTuiGraphics {

enum class Protocol : std::uint8_t {
    None,  /* Metadata text only */
    Cells, /* Half-block preview in the cell grid */
    Kitty,
    ITerm2,
    Sixel,
};

/** @brief Parsed answers to the startup queries. */
struct ProbeReply
{
    bool       answered = false; /* The DA1 sentinel arrived */
    bool       kitty    = false;
    bool       sixel    = false;
    QSize      cell;
    bool       light = false; /* Background color reply is bright */
    QString    terminal;
    QByteArray typeAhead; /* Key bytes read between the replies */
};

ProbeReply parseProbeReply(const QByteArray &bytes);
Protocol   selectProtocol(const QProcessEnvironment &env, const ProbeReply &reply);

/** @brief Query the terminal once. Must run before anything else reads stdin. */
void       probeTerminal();
void       setProbeReply(const ProbeReply &reply);
QByteArray takeTypeAhead();

Protocol protocol();
/** @brief Bitmap written into the cells at the cursor, erased by later writes. */
bool isInlineBitmap(Protocol protocol);
/** @brief Kitty bitmap placed through text cells, the only placement a multiplexer keeps. */
bool usesPlaceholders();
/** @brief Wrap a sequence for the terminal outside tmux. */
QString passthrough(const QString &sequence);
/** @brief Encode @p image as one DCS sequence with an adaptive 256 color palette. */
QByteArray encodeSixel(const QImage &image);
bool       hasTrueColor();
bool       hasLightBackground();
/** @brief Tallest picture, so an inline bitmap can fit the viewport whole. */
int maxImageRows();
/** @brief Cell size in pixels: the window size report, the probe, or 8x16. */
QSize cellSize();

/**
 * @brief Tracks the image data the terminal stores, oldest placed first.
 * @details The terminal silently evicts past its own quota, which would leave
 *          placements pointing at nothing, so this evicts first.
 */
class ImageStore
{
public:
    static ImageStore &instance();

    quint32 allocateId();
    bool    isResident(quint32 id) const;
    /** @return Ids to delete before transmitting @p id; never a placed image. */
    QList<quint32> admit(quint32 id, qint64 bytes);
    void           setPlaced(quint32 id, bool placed);
    void           forget(quint32 id);
    void           setLimitForTest(qint64 bytes) { limit = bytes; }

private:
    struct Resident
    {
        quint32 id;
        qint64  bytes;
    };
    QList<Resident> residents;
    QSet<quint32>   placedIds;
    qint64          residentBytes = 0;
    qint64          limit         = 200 * 1024 * 1024;
    quint32         nextId        = 1;
};

} // namespace QTuiGraphics

#endif // QTUIGRAPHICS_H
