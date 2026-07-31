// Tests for turning a clipboard/drop payload into the images a request carries.
// Needs QtGui for QImage, but no widgets, no display and no QGuiApplication:
// everything attach_images() does is in-memory, which is why the encode rules
// live in a library rather than inside the window.

#include "gui/image_attach.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QMimeData>
#include <QUrl>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "test_harness.hpp"

using namespace moocode;
using namespace moocode::gui;

namespace {

// A recognisable test bitmap: a diagonal so a scaled copy is still obviously
// the same picture, on a filled ground so PNG cannot compress it to nothing.
QImage checker(int w, int h) {
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(Qt::darkBlue);
    for (int i = 0; i < std::min(w, h); ++i) img.setPixel(i, i, 0xffcc00);
    return img;
}

QByteArray encode(const QImage& img, const char* fmt) {
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, fmt);
    return out;
}

// Base64 decodes back to bytes an image reader recognises as `fmt`.
bool decodes_as(const ImageBlock& b, const char* fmt) {
    const QByteArray raw =
        QByteArray::fromBase64(QByteArray::fromStdString(b.base64_data));
    if (raw.isEmpty()) return false;
    return !QImage::fromData(raw, fmt).isNull();
}

QImage decoded(const ImageBlock& b) {
    return QImage::fromData(
        QByteArray::fromBase64(QByteArray::fromStdString(b.base64_data)));
}

// A temporary file holding `bytes`, removed by the destructor.
class TempFile {
public:
    TempFile(const std::string& suffix, const QByteArray& bytes) {
        path_ = std::filesystem::temp_directory_path() /
                ("moo_attach_test" + std::to_string(counter()++) + suffix);
        std::ofstream f(path_, std::ios::binary);
        f.write(bytes.constData(), bytes.size());
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    QString qpath() const { return QString::fromStdString(path_.string()); }
    QUrl url() const { return QUrl::fromLocalFile(qpath()); }

private:
    static int& counter() {
        static int n = 0;
        return n;
    }
    std::filesystem::path path_;
};

}  // namespace

TEST("nothing attachable in an empty or text-only payload") {
    CHECK(!has_attachable_image(nullptr));
    QMimeData md;
    CHECK(!has_attachable_image(&md));
    md.setText(QStringLiteral("just some prose"));
    CHECK(!has_attachable_image(&md));
    CHECK(attach_images(&md).images.empty());
    CHECK(attach_images(&md).errors.isEmpty());
}

TEST("a clipboard bitmap attaches as one image") {
    QMimeData md;
    md.setImageData(checker(40, 30));
    CHECK(has_attachable_image(&md));

    AttachResult r = attach_images(&md);
    CHECK(r.errors.isEmpty());
    CHECK_EQ(r.images.size(), std::size_t{1});
    CHECK_EQ(r.images[0].block.media_type, std::string("image/png"));
    CHECK(decodes_as(r.images[0].block, "PNG"));
    CHECK(!r.images[0].preview.isNull());
    // Ids are the composer's to assign; the encoder leaves them alone.
    CHECK_EQ(r.images[0].id, 0);
}

TEST("the original compressed bytes are forwarded when there are any") {
    const QImage img = checker(50, 40);
    const QByteArray jpeg = encode(img, "JPEG");
    CHECK(!jpeg.isEmpty());

    QMimeData md;
    md.setImageData(img);  // what a paste normally offers
    md.setData(QStringLiteral("image/jpeg"), jpeg);

    AttachResult r = attach_images(&md);
    CHECK_EQ(r.images.size(), std::size_t{1});
    // Passed through, not re-encoded: same media type, same bytes.
    CHECK_EQ(r.images[0].block.media_type, std::string("image/jpeg"));
    CHECK_EQ(r.images[0].block.base64_data,
             std::string(jpeg.toBase64().constData()));
}

TEST("an oversized bitmap is downscaled rather than sent whole") {
    // Well past the long-edge cap, and with compressed bytes on offer — the
    // rescale has to win over the passthrough, or the cap would do nothing.
    const QImage big = checker(4000, 2000);
    QMimeData md;
    md.setImageData(big);
    md.setData(QStringLiteral("image/png"), encode(big, "PNG"));

    AttachResult r = attach_images(&md);
    CHECK_EQ(r.images.size(), std::size_t{1});
    CHECK_EQ(r.images[0].block.media_type, std::string("image/png"));
    const QImage out = decoded(r.images[0].block);
    CHECK(!out.isNull());
    CHECK(std::max(out.width(), out.height()) <= 1568);
    // Aspect ratio kept (2:1, within a pixel of rounding).
    CHECK(out.width() > out.height());
}

TEST("compressed bytes with no decoded bitmap still attach") {
    QMimeData md;
    md.setData(QStringLiteral("image/png"), encode(checker(20, 20), "PNG"));
    CHECK(has_attachable_image(&md));
    AttachResult r = attach_images(&md);
    CHECK_EQ(r.images.size(), std::size_t{1});
    CHECK(decodes_as(r.images[0].block, "PNG"));
}

TEST("an image file URL is read from disk, keeping its own media type") {
    TempFile jpg(".jpg", encode(checker(60, 45), "JPEG"));
    QMimeData md;
    md.setUrls({jpg.url()});
    CHECK(has_attachable_image(&md));

    AttachResult r = attach_images(&md);
    CHECK(r.errors.isEmpty());
    CHECK_EQ(r.images.size(), std::size_t{1});
    CHECK_EQ(r.images[0].block.media_type, std::string("image/jpeg"));
    CHECK(!r.images[0].block.base64_data.empty());
    // The label is the file name, so the chip says which file it was.
    CHECK(r.images[0].label.endsWith(QStringLiteral(".jpg")));
    CHECK(!r.images[0].preview.isNull());
}

TEST("several dropped files attach in order; non-images are ignored") {
    TempFile a(".png", encode(checker(10, 10), "PNG"));
    TempFile b(".jpg", encode(checker(12, 12), "JPEG"));
    TempFile c(".txt", QByteArray("not an image"));
    QMimeData md;
    md.setUrls({a.url(), c.url(), b.url()});

    AttachResult r = attach_images(&md);
    CHECK(r.errors.isEmpty());
    CHECK_EQ(r.images.size(), std::size_t{2});
    CHECK_EQ(r.images[0].block.media_type, std::string("image/png"));
    CHECK_EQ(r.images[1].block.media_type, std::string("image/jpeg"));
}

TEST("a file drag beats the bitmap preview shipped alongside it") {
    // Browsers offer both; attaching both would send the same picture twice.
    TempFile png(".png", encode(checker(24, 24), "PNG"));
    QMimeData md;
    md.setUrls({png.url()});
    md.setImageData(checker(24, 24));

    AttachResult r = attach_images(&md);
    CHECK_EQ(r.images.size(), std::size_t{1});
    CHECK(r.images[0].label.endsWith(QStringLiteral(".png")));
}

TEST("an unreadable image file is reported, not silently dropped") {
    QMimeData md;
    md.setUrls({QUrl::fromLocalFile(
        QStringLiteral("/nonexistent/moo_attach_missing.png"))});
    // The extension is what makes it attachable; the read is what fails.
    CHECK(has_attachable_image(&md));

    AttachResult r = attach_images(&md);
    CHECK(r.images.empty());
    CHECK_EQ(r.errors.size(), qsizetype{1});
    CHECK(r.errors[0].contains(QStringLiteral("moo_attach_missing.png")));
}

TEST("a remote URL is not attachable") {
    QMimeData md;
    md.setUrls({QUrl(QStringLiteral("https://example.com/cat.png"))});
    CHECK(!has_attachable_image(&md));
    CHECK(attach_images(&md).images.empty());
}
