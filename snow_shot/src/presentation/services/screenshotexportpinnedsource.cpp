#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotrecognitionimage.h"

#include "snow_shot/presentation/screenshotdefaultstyles.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QList>
#include <QColorSpace>
#include <QMutex>
#include <QMutexLocker>

#include <utility>

namespace {
struct PinnedPixels final {
    explicit PinnedPixels(ScreenshotPinnedViewportExportSource value) : source(std::move(value)) {}

    QMutex mutex;
    std::optional<ScreenshotPinnedViewportExportSource> source;
    QImage image;
};

QImage renderPinnedViewport(const ScreenshotPinnedViewportExportSource& source,
                            const ScreenshotExportCancellation& cancellation) {
    if (source.backgroundImage.isNull() || !source.backgroundCanvasRect.isValid() ||
        source.backgroundCanvasRect.isEmpty() || !source.contentPixelSize.isValid() ||
        source.contentPixelSize.isEmpty()) {
        return {};
    }
    QImage content;
    const QList<CanvasExportSource> sources{
        CanvasExportSource{source.backgroundImage, source.backgroundCanvasRect}};
    if (source.documentSession.isEmpty()) {
        // An empty document has nothing to synchronize, so the export surface is
        // only a projection. Ask the engine for it instead of hand-rolling the
        // painter: the canvas sets a temporary raster scale, and re-deriving that
        // here put every scaled pixel one level off from the runtime path.
        content = SnowCanvasRuntime::projectSourcesToImage(source.backgroundCanvasRect,
                                                           source.contentPixelSize, sources);
    } else {
        // Imported document and reconstruction pixels belong to this export.
        SnowCanvasRuntime runtime(
            SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasStyleDefaults()});
        if (!runtime.isValid() || !runtime.restoreDocumentSession(source.documentSession) ||
            cancellation.isCancellationRequested())
            return {};
        runtime.restoreSmartEraseSnapshot(source.smartErase);
        if (cancellation.isCancellationRequested())
            return {};
        content =
            runtime.renderToImage(source.backgroundCanvasRect, source.contentPixelSize, sources);
    }
    if (cancellation.isCancellationRequested())
        return {};
    content.setColorSpace(source.backgroundImage.colorSpace());
    return content.isNull() ? QImage{}
                            : ScreenshotResultCompositor::compose(content, source.resultStyle, 1.0,
                                                                  source.outputOpacity);
}
} // namespace

ScreenshotExportSource
ScreenshotExportSource::fromPinnedViewport(ScreenshotPinnedViewportExportSource source) {
    auto placement = source.clipboardPlacement;
    auto appearance = source.clipboardAppearance;
    auto pixels = std::make_shared<PinnedPixels>(std::move(source));
    auto result = fromProducer(
        [pixels = std::move(pixels)](const ScreenshotExportCancellation& cancellation) {
            QMutexLocker lock(&pixels->mutex);
            if (cancellation.isCancellationRequested())
                return QImage{};
            if (!pixels->image.isNull())
                return pixels->image;
            QImage image = renderPinnedViewport(*pixels->source, cancellation);
            if (!image.isNull()) {
                pixels->image = image;
                // Copies of this source share the immutable rendered pixels. Once
                // ready, retain no background, document, or reconstruction donors.
                pixels->source.reset();
            }
            return image;
        });
    result.m_clipboardPlacement = std::move(placement);
    result.m_clipboardAppearance = std::move(appearance);
    return result;
}

ScreenshotExportSource
ScreenshotExportSource::fromRecognitionImage(ScreenshotRecognitionImageSnapshot snapshot) {
    return fromProducer(
        [snapshot = std::move(snapshot)](const ScreenshotExportCancellation& cancellation) {
            return renderScreenshotRecognitionImage(
                snapshot, [&]() { return cancellation.isCancellationRequested(); });
        });
}
