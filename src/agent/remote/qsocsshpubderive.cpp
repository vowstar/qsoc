// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsshpubderive.h"

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/rsa.h>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>

#include <cstring>

namespace {

constexpr auto   kOpensshV1Header  = "-----BEGIN OPENSSH PRIVATE KEY-----";
constexpr auto   kOpensshV1Footer  = "-----END OPENSSH PRIVATE KEY-----";
constexpr auto   kOpensshAuthMagic = "openssh-key-v1";
constexpr size_t kAuthMagicSize    = 15; /* includes trailing NUL */

/* Zero a QByteArray's underlying storage before letting it go out of
 * scope. The key contents should not survive in freed heap blocks. */
void secureClear(QByteArray &buf)
{
    if (buf.size() > 0) {
        mbedtls_platform_zeroize(buf.data(), static_cast<size_t>(buf.size()));
    }
    buf.clear();
}

/* Append a 4-byte big-endian length followed by raw bytes, matching the
 * `string` type used in the SSH wire format and in openssh-key-v1. */
void appendSshString(QByteArray &out, const QByteArray &value)
{
    const quint32 len = static_cast<quint32>(value.size());
    out.append(static_cast<char>((len >> 24) & 0xFF));
    out.append(static_cast<char>((len >> 16) & 0xFF));
    out.append(static_cast<char>((len >> 8) & 0xFF));
    out.append(static_cast<char>(len & 0xFF));
    out.append(value);
}

/* Encode an mbedtls_mpi as an SSH mpint: a length-prefixed two's-complement
 * big-endian integer with an optional 0x00 prefix so positive numbers whose
 * top bit is set are not misread as negative. */
int appendSshMpi(QByteArray &out, const mbedtls_mpi &value)
{
    size_t len = mbedtls_mpi_size(&value);
    if (len == 0) {
        appendSshString(out, QByteArray());
        return 0;
    }
    QByteArray raw(static_cast<int>(len), '\0');
    int rc = mbedtls_mpi_write_binary(&value, reinterpret_cast<unsigned char *>(raw.data()), len);
    if (rc != 0) {
        return rc;
    }
    /* Strip leading zero bytes, then re-insert a single one if the high
     * bit of the next byte is set (SSH mpint is signed). */
    int start = 0;
    while (start < raw.size() - 1 && raw.at(start) == '\0') {
        ++start;
    }
    QByteArray trimmed = raw.mid(start);
    if (!trimmed.isEmpty() && (static_cast<unsigned char>(trimmed.at(0)) & 0x80U) != 0U) {
        trimmed.prepend('\0');
    }
    appendSshString(out, trimmed);
    secureClear(raw);
    return 0;
}

/* Small bounds-checked reader for the openssh-key-v1 blob format. Every
 * helper returns false on underflow so the caller can bail out instead
 * of reading past the buffer. */
class SshBlobReader
{
public:
    SshBlobReader(const unsigned char *data, size_t size)
        : m_data(data)
        , m_size(size)
        , m_pos(0)
    {}

    bool readU32(quint32 &value)
    {
        if (m_pos + 4 > m_size) {
            return false;
        }
        value = (static_cast<quint32>(m_data[m_pos]) << 24)
                | (static_cast<quint32>(m_data[m_pos + 1]) << 16)
                | (static_cast<quint32>(m_data[m_pos + 2]) << 8)
                | static_cast<quint32>(m_data[m_pos + 3]);
        m_pos += 4;
        return true;
    }

    bool readString(QByteArray &out)
    {
        quint32 len = 0;
        if (!readU32(len)) {
            return false;
        }
        if (m_pos + len > m_size) {
            return false;
        }
        out = QByteArray(reinterpret_cast<const char *>(m_data + m_pos), static_cast<int>(len));
        m_pos += len;
        return true;
    }

    bool skipString()
    {
        quint32 len = 0;
        if (!readU32(len)) {
            return false;
        }
        if (m_pos + len > m_size) {
            return false;
        }
        m_pos += len;
        return true;
    }

    size_t remaining() const { return m_size - m_pos; }

private:
    const unsigned char *m_data;
    size_t               m_size;
    size_t               m_pos;
};

/* openssh-key-v1 path: parse the unencrypted container and reuse the
 * public-key blob that is already embedded in the SSH wire format.
 * Returns the authorized_keys line, or an empty QString on any error or
 * on encrypted input (which we refuse by design). */
QString deriveFromOpensshV1(const QByteArray &fileContents)
{
    int headerIndex = fileContents.indexOf(kOpensshV1Header);
    int footerIndex = fileContents.indexOf(kOpensshV1Footer);
    if (headerIndex < 0 || footerIndex <= headerIndex) {
        return {};
    }
    const int        bodyStart = headerIndex + static_cast<int>(std::strlen(kOpensshV1Header));
    const QByteArray body
        = fileContents.mid(bodyStart, footerIndex - bodyStart).replace('\r', "").replace('\n', "");
    if (body.isEmpty()) {
        return {};
    }

    /* Upper bound: base64 decodes to at most 3/4 of the input size. */
    const size_t maxLen = (static_cast<size_t>(body.size()) * 3U) / 4U + 4U;
    QByteArray   decoded(static_cast<int>(maxLen), '\0');
    size_t       outLen = 0;
    const int    rc     = mbedtls_base64_decode(
        reinterpret_cast<unsigned char *>(decoded.data()),
        maxLen,
        &outLen,
        reinterpret_cast<const unsigned char *>(body.constData()),
        static_cast<size_t>(body.size()));
    if (rc != 0) {
        secureClear(decoded);
        return {};
    }
    decoded.resize(static_cast<int>(outLen));

    if (static_cast<size_t>(decoded.size()) < kAuthMagicSize
        || std::memcmp(decoded.constData(), kOpensshAuthMagic, kAuthMagicSize) != 0) {
        secureClear(decoded);
        return {};
    }

    SshBlobReader reader(
        reinterpret_cast<const unsigned char *>(decoded.constData()) + kAuthMagicSize,
        static_cast<size_t>(decoded.size()) - kAuthMagicSize);

    QByteArray cipher;
    QByteArray kdf;
    QByteArray kdfOptions;
    quint32    keyCount = 0;
    QByteArray pubBlob;
    if (!reader.readString(cipher) || !reader.readString(kdf) || !reader.readString(kdfOptions)
        || !reader.readU32(keyCount) || keyCount != 1 || !reader.readString(pubBlob)) {
        secureClear(decoded);
        return {};
    }
    /* Refuse encrypted keys without attempting a passphrase prompt. */
    if (cipher != "none" || kdf != "none") {
        secureClear(decoded);
        return {};
    }

    /* The first field inside the public blob is the key type string,
     * which is also what goes in front of the base64 line. */
    SshBlobReader pubReader(
        reinterpret_cast<const unsigned char *>(pubBlob.constData()),
        static_cast<size_t>(pubBlob.size()));
    QByteArray keyType;
    if (!pubReader.readString(keyType) || keyType.isEmpty()) {
        secureClear(decoded);
        return {};
    }

    const size_t encMax = 4U * static_cast<size_t>(pubBlob.size()) / 3U + 4U;
    QByteArray   encoded(static_cast<int>(encMax), '\0');
    size_t       encLen = 0;
    if (mbedtls_base64_encode(
            reinterpret_cast<unsigned char *>(encoded.data()),
            encMax,
            &encLen,
            reinterpret_cast<const unsigned char *>(pubBlob.constData()),
            static_cast<size_t>(pubBlob.size()))
        != 0) {
        secureClear(decoded);
        return {};
    }
    encoded.resize(static_cast<int>(encLen));

    QString result = QString::fromLatin1(keyType) + QLatin1Char(' ') + QString::fromLatin1(encoded);
    secureClear(decoded);
    return result;
}

/* Classic PEM path: ask mbedTLS to parse the file and then pull N/E for
 * RSA or the curve point for ECDSA out of the parsed context. */
QString deriveFromPem(const QByteArray &fileContents)
{
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_context       pk;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&rng);
    mbedtls_pk_init(&pk);

    const auto label = QByteArrayLiteral("qsoc-ssh-pubderive");
    if (mbedtls_ctr_drbg_seed(
            &rng,
            mbedtls_entropy_func,
            &entropy,
            reinterpret_cast<const unsigned char *>(label.constData()),
            static_cast<size_t>(label.size()))
        != 0) {
        mbedtls_pk_free(&pk);
        mbedtls_ctr_drbg_free(&rng);
        mbedtls_entropy_free(&entropy);
        return {};
    }

    /* mbedtls_pk_parse_key needs a NUL-terminated buffer for PEM input,
     * and the length must include that terminator. Copy the file into a
     * padded buffer we also zeroise before releasing. */
    QByteArray padded = fileContents;
    padded.append('\0');
    const int pkRc = mbedtls_pk_parse_key(
        &pk,
        reinterpret_cast<const unsigned char *>(padded.constData()),
        static_cast<size_t>(padded.size()),
        nullptr,
        0,
        mbedtls_ctr_drbg_random,
        &rng);
    secureClear(padded);
    if (pkRc != 0) {
        mbedtls_pk_free(&pk);
        mbedtls_ctr_drbg_free(&rng);
        mbedtls_entropy_free(&entropy);
        return {};
    }

    QString                 result;
    const mbedtls_pk_type_t type = mbedtls_pk_get_type(&pk);
    if (type == MBEDTLS_PK_RSA) {
        mbedtls_rsa_context *rsa = mbedtls_pk_rsa(pk);
        mbedtls_mpi          nBig;
        mbedtls_mpi          eBig;
        mbedtls_mpi_init(&nBig);
        mbedtls_mpi_init(&eBig);
        int rc = mbedtls_rsa_export(rsa, &nBig, nullptr, nullptr, nullptr, &eBig);
        if (rc == 0) {
            QByteArray blob;
            appendSshString(blob, QByteArrayLiteral("ssh-rsa"));
            rc = appendSshMpi(blob, eBig);
            if (rc == 0) {
                rc = appendSshMpi(blob, nBig);
            }
            if (rc == 0) {
                const size_t encMax = 4U * static_cast<size_t>(blob.size()) / 3U + 4U;
                QByteArray   encoded(static_cast<int>(encMax), '\0');
                size_t       encLen = 0;
                if (mbedtls_base64_encode(
                        reinterpret_cast<unsigned char *>(encoded.data()),
                        encMax,
                        &encLen,
                        reinterpret_cast<const unsigned char *>(blob.constData()),
                        static_cast<size_t>(blob.size()))
                    == 0) {
                    encoded.resize(static_cast<int>(encLen));
                    result = QStringLiteral("ssh-rsa ") + QString::fromLatin1(encoded);
                }
            }
            secureClear(blob);
        }
        mbedtls_mpi_free(&nBig);
        mbedtls_mpi_free(&eBig);
    } else if (type == MBEDTLS_PK_ECKEY) {
        mbedtls_ecp_keypair *ec = mbedtls_pk_ec(pk);
        mbedtls_ecp_group    grp;
        mbedtls_ecp_point    pub;
        mbedtls_ecp_group_init(&grp);
        mbedtls_ecp_point_init(&pub);
        /* Copy out the curve id and point rather than holding references
         * into libmbedtls internals; safer if the underlying keypair
         * layout changes between releases. */
        int rc = mbedtls_ecp_group_load(&grp, mbedtls_ecp_keypair_get_group_id(ec));
        if (rc == 0) {
            rc = mbedtls_ecp_export(ec, nullptr, nullptr, &pub);
        }
        const char *curveName    = nullptr;
        const char *sshAlgorithm = nullptr;
        if (rc == 0) {
            switch (grp.id) {
            case MBEDTLS_ECP_DP_SECP256R1:
                curveName    = "nistp256";
                sshAlgorithm = "ecdsa-sha2-nistp256";
                break;
            case MBEDTLS_ECP_DP_SECP384R1:
                curveName    = "nistp384";
                sshAlgorithm = "ecdsa-sha2-nistp384";
                break;
            case MBEDTLS_ECP_DP_SECP521R1:
                curveName    = "nistp521";
                sshAlgorithm = "ecdsa-sha2-nistp521";
                break;
            default:
                break;
            }
        }
        if (curveName != nullptr && sshAlgorithm != nullptr) {
            unsigned char pointBuf[1 + 2 * 66];
            size_t        pointLen = 0;
            rc                     = mbedtls_ecp_point_write_binary(
                &grp, &pub, MBEDTLS_ECP_PF_UNCOMPRESSED, &pointLen, pointBuf, sizeof pointBuf);
            if (rc == 0) {
                QByteArray blob;
                appendSshString(blob, QByteArray(sshAlgorithm));
                appendSshString(blob, QByteArray(curveName));
                appendSshString(
                    blob,
                    QByteArray(reinterpret_cast<const char *>(pointBuf), static_cast<int>(pointLen)));
                const size_t encMax = 4U * static_cast<size_t>(blob.size()) / 3U + 4U;
                QByteArray   encoded(static_cast<int>(encMax), '\0');
                size_t       encLen = 0;
                if (mbedtls_base64_encode(
                        reinterpret_cast<unsigned char *>(encoded.data()),
                        encMax,
                        &encLen,
                        reinterpret_cast<const unsigned char *>(blob.constData()),
                        static_cast<size_t>(blob.size()))
                    == 0) {
                    encoded.resize(static_cast<int>(encLen));
                    result = QString::fromLatin1(sshAlgorithm) + QLatin1Char(' ')
                             + QString::fromLatin1(encoded);
                }
                secureClear(blob);
            }
            mbedtls_platform_zeroize(pointBuf, sizeof pointBuf);
        }
        mbedtls_ecp_point_free(&pub);
        mbedtls_ecp_group_free(&grp);
    }

    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&rng);
    mbedtls_entropy_free(&entropy);
    return result;
}

} // namespace

QString QSocSshPubDerive::fromPrivateKeyFile(const QString &privateKeyPath)
{
    QFileInfo info(privateKeyPath);
    if (!info.exists() || !info.isFile()) {
        return {};
    }
    QFile file(privateKeyPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QByteArray contents = file.readAll();
    file.close();
    if (contents.isEmpty()) {
        return {};
    }

    QString result;
    if (contents.contains(kOpensshV1Header)) {
        result = deriveFromOpensshV1(contents);
    } else {
        result = deriveFromPem(contents);
    }
    secureClear(contents);
    return result;
}
