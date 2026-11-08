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
 *          from terminal replies and the attached clients.
 */
namespace QTuiGraphics {

enum class Protocol : std::uint8_t {
    None,  /* Metadata text only */
    Cells, /* Half-block preview in the cell grid */
    Kitty,
    ITerm2,
    Sixel,
};

enum class Support : std::uint8_t { Unknown, No, Yes };
enum class Evidence : std::uint8_t { None, Override, Environment, Terminal, Tmux };

struct Capabilities
{
    Protocol protocol     = Protocol::None;
    Evidence evidence     = Evidence::None;
    Support  placeholders = Support::Unknown;
};

/** @brief Graphics evidence from replies or attached clients. */
struct ProbeReply
{
    bool       answered     = false; /* The DA1 sentinel arrived */
    Support    kitty        = Support::Unknown;
    Support    sixel        = Support::Unknown;
    Support    placeholders = Support::Unknown;
    Support    passthrough  = Support::Unknown;
    Support    cells        = Support::Unknown;
    Evidence   evidence     = Evidence::Terminal;
    QSize      cell;
    bool       light = false; /* Background color reply is bright */
    QString    terminal;
    QByteArray typeAhead; /* Key bytes read between the replies */
};

ProbeReply   parseProbeReply(const QByteArray &bytes);
Protocol     selectProtocol(const QProcessEnvironment &env, const ProbeReply &reply);
Capabilities selectCapabilities(const QProcessEnvironment &env, const ProbeReply &reply);

/** @brief Query the terminal once. Must run before anything else reads stdin. */
void       probeTerminal();
void       setProbeReply(const ProbeReply &reply);
QByteArray takeTypeAhead();
/** @brief Accept a complete reply from the input owner without reading stdin. */
void handleProbeReply(const QByteArray &bytes);
/** @brief Check attached clients asynchronously while the TUI is running. */
void    refreshTerminal();
quint64 revision();

Protocol     protocol();
Capabilities capabilities();
/** @brief Bitmap written into the cells at the cursor, erased by later writes. */
bool isInlineBitmap(Protocol value);
/** @brief Whether Kitty pictures use tmux Unicode placeholders. */
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
