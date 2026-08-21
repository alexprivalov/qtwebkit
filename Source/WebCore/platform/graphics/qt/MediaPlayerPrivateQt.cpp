/*
    Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies)

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#include "config.h"
#include "MediaPlayerPrivateQt.h"

#include "Frame.h"
#include "FrameLoader.h"
#include "FrameView.h"
#include "GraphicsContext.h"
#include "GraphicsLayer.h"
#include "HTMLMediaElement.h"
#include "Logging.h"
#include "NetworkingContext.h"
#include "NotImplemented.h"
#include "RenderVideo.h"

#include <QBuffer>
#include <QMediaPlayerControl>
#include <QMediaService>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkRequest>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QTime>
#include <QTimer>
#include <QUrl>
#include <limits>
#include <qmediametadata.h>
#include <qmultimedia.h>
#include <wtf/HashSet.h>
#include <wtf/text/CString.h>

#include "texmap/TextureMapper.h"

using namespace WTF;

// Wipes its payload on destruction. This covers only the copy we own: the QNetworkReply that
// produced the bytes keeps its own buffer, and the platform media backend may take a further copy
// of whatever it reads. Treat it as reducing the plaintext residue, not as removing it.
class WipingMediaBuffer final : public QBuffer {
public:
    explicit WipingMediaBuffer(QObject* parent)
        : QBuffer(parent)
    {
    }

    ~WipingMediaBuffer() override
    {
        close();
        QByteArray& bytes = buffer();
        volatile char* data = bytes.data();
        for (int i = 0; i < bytes.size(); ++i)
            data[i] = 0;
    }
};

namespace WebCore {

std::unique_ptr<MediaPlayerPrivateInterface> MediaPlayerPrivateQt::create(MediaPlayer* player)
{
    return std::make_unique<MediaPlayerPrivateQt>(player);
}

void MediaPlayerPrivateQt::registerMediaEngine(MediaEngineRegistrar registrar)
{
    registrar(create, getSupportedTypes, supportsType, 0, 0, 0, 0);
}

void MediaPlayerPrivateQt::getSupportedTypes(HashSet<String, ASCIICaseInsensitiveHash>& supported)
{
    QStringList types = QMediaPlayer::supportedMimeTypes();

    for (int i = 0; i < types.size(); i++) {
        QString mime = types.at(i);
        if (mime.startsWith(QString::fromLatin1("audio/")) || mime.startsWith(QString::fromLatin1("video/")))
            supported.add(mime);
    }
}

MediaPlayer::SupportsType MediaPlayerPrivateQt::supportsType(const MediaEngineSupportParameters& parameters)
{
    if (parameters.isMediaStream || parameters.isMediaSource)
        return MediaPlayer::IsNotSupported;

    if (!parameters.type.raw().startsWithIgnoringASCIICase("audio/") && !parameters.type.raw().startsWithIgnoringASCIICase("video/"))
        return MediaPlayer::IsNotSupported;

    // Parse and trim codecs
    QStringList codecList;
    for (const String& codec : parameters.type.codecs())
        codecList.append(codec);

    if (QMediaPlayer::hasSupport(parameters.type.containerType(), codecList) >= QMultimedia::ProbablySupported)
        return MediaPlayer::IsSupported;

    return MediaPlayer::MayBeSupported;
}

MediaPlayerPrivateQt::MediaPlayerPrivateQt(MediaPlayer* player)
    : m_webCorePlayer(player)
    , m_mediaPlayer(new QMediaPlayer)
    , m_mediaPlayerControl(0)
    , m_networkState(MediaPlayer::Empty)
    , m_readyState(MediaPlayer::HaveNothing)
    , m_currentSize(0, 0)
    , m_naturalSize(RenderVideo::defaultSize())
    , m_isSeeking(false)
    , m_resumePlaybackAfterSeek(false)
    , m_seekGeneration(0)
    , m_composited(false)
    , m_preload(MediaPlayer::Auto)
    , m_bytesLoadedAtLastDidLoadingProgress(0)
    , m_suppressNextPlaybackChanged(false)
    , m_prerolling(false)
{
    m_mediaPlayer->setVideoOutput(this);

    // Signal Handlers
    connect(m_mediaPlayer, SIGNAL(mediaStatusChanged(QMediaPlayer::MediaStatus)),
            this, SLOT(mediaStatusChanged(QMediaPlayer::MediaStatus)));
    connect(m_mediaPlayer, SIGNAL(stateChanged(QMediaPlayer::State)),
            this, SLOT(stateChanged(QMediaPlayer::State)));
    connect(m_mediaPlayer, SIGNAL(error(QMediaPlayer::Error)),
            this, SLOT(handleError(QMediaPlayer::Error)));
    connect(m_mediaPlayer, SIGNAL(bufferStatusChanged(int)),
            this, SLOT(bufferStatusChanged(int)));
    connect(m_mediaPlayer, SIGNAL(durationChanged(qint64)),
            this, SLOT(durationChanged(qint64)));
    connect(m_mediaPlayer, SIGNAL(positionChanged(qint64)),
            this, SLOT(positionChanged(qint64)));
    connect(m_mediaPlayer, SIGNAL(volumeChanged(int)),
            this, SLOT(volumeChanged(int)));
    connect(m_mediaPlayer, SIGNAL(mutedChanged(bool)),
            this, SLOT(mutedChanged(bool)));
    connect(this, SIGNAL(surfaceFormatChanged(const QVideoSurfaceFormat&)),
            this, SLOT(surfaceFormatChanged(const QVideoSurfaceFormat&)));

    // Grab the player control
    if (QMediaService* service = m_mediaPlayer->service()) {
        m_mediaPlayerControl = qobject_cast<QMediaPlayerControl *>(
                service->requestControl(QMediaPlayerControl_iid));
    }
}

MediaPlayerPrivateQt::~MediaPlayerPrivateQt()
{
    m_mediaPlayer->disconnect(this);
    m_mediaPlayer->stop();
    m_mediaPlayer->setMedia(QMediaContent());

    delete m_mediaPlayer;
}

bool MediaPlayerPrivateQt::hasVideo() const
{
    return m_mediaPlayer->isVideoAvailable();
}

bool MediaPlayerPrivateQt::hasAudio() const
{
    return true;
}

void MediaPlayerPrivateQt::load(const String& url)
{
    m_mediaUrl = url;

    // QtMultimedia does not have an API to throttle loading
    // so we handle this ourselves by delaying the load
    if (m_preload == MediaPlayer::None) {
        m_delayingLoad = true;
        return;
    }

    commitLoad(url);
}

void MediaPlayerPrivateQt::commitLoad(const String& url)
{
    // We are now loading
    if (m_networkState != MediaPlayer::Loading) {
        m_networkState = MediaPlayer::Loading;
        m_webCorePlayer->networkStateChanged();
    }

    // And we don't have any data yet
    if (m_readyState != MediaPlayer::HaveNothing) {
        m_readyState = MediaPlayer::HaveNothing;
        m_webCorePlayer->readyStateChanged();
    }

    URL kUrl({ }, url);
    const QUrl rUrl = kUrl;
    const QString scheme = rUrl.scheme().toLower();

    // Construct the media content with a network request if the resource is http[s]
    if (scheme == QString::fromLatin1("http") || scheme == QString::fromLatin1("https")) {
        QNetworkRequest request = QNetworkRequest(rUrl);

        // Grab the current document
        Document* document = m_webCorePlayer->client().mediaPlayerOwningDocument();

        // Grab the frame and network manager
        Frame* frame = document ? document->frame() : 0;
        FrameLoader* frameLoader = frame ? &frame->loader() : 0;
        QNetworkAccessManager* manager = frameLoader ? frameLoader->networkingContext()->networkAccessManager() : 0;

        if (manager) {
            // Set the cookies
            QNetworkCookieJar* jar = manager->cookieJar();
            QList<QNetworkCookie> cookies = jar->cookiesForUrl(rUrl);

            // Don't set the header if there are no cookies.
            // This prevents a warning from being emitted.
            if (!cookies.isEmpty())
                request.setHeader(QNetworkRequest::CookieHeader, QVariant::fromValue(cookies));

            // Set the refferer, but not when requesting insecure content from a secure page
            QUrl documentUrl = QUrl(QString(document->documentURI()));
            if (documentUrl.scheme().toLower() == QString::fromLatin1("http") || scheme == QString::fromLatin1("https"))
                request.setRawHeader("Referer", documentUrl.toEncoded());

            // Set the user agent
            request.setRawHeader("User-Agent", frameLoader->userAgent(rUrl).utf8().data());
        }

        m_mediaPlayer->setMedia(QMediaContent(request));
        startPlayback();
        return;
    }

    // Anything else - in practice the application's own scheme, serving media from inside a
    // book. Handing such a URL to QMediaPlayer does nothing at all: the backend resolves it
    // itself and knows nothing of the scheme, so the resource is never even requested. Fetch it
    // through the frame's network stack, which does know, and give the player the bytes.
    Document* document = m_webCorePlayer->client().mediaPlayerOwningDocument();
    Frame* frame = document ? document->frame() : nullptr;
    FrameLoader* frameLoader = frame ? &frame->loader() : nullptr;
    QNetworkAccessManager* manager = frameLoader ? frameLoader->networkingContext()->networkAccessManager() : nullptr;
    if (!manager) {
        reportNetworkError();
        return;
    }

    QNetworkReply* reply = manager->get(QNetworkRequest(rUrl));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, rUrl]() {
        if (reply->error() != QNetworkReply::NoError) {
            reply->deleteLater();
            reportNetworkError();
            return;
        }

        // The whole asset is held in memory for the lifetime of the element. That is what buys
        // seeking: a QNetworkReply is sequential, so handing it to the player directly would
        // leave the scrubber dead. The cost is a contiguous allocation the size of the media in
        // a 32-bit address space.
        QByteArray payload = reply->readAll();
        reply->deleteLater();

        // Parented, so the wipe is guaranteed to run: deleteLater() alone would leave the buffer
        // alive if nothing pumps the event loop again.
        auto* mediaBuffer = new WipingMediaBuffer(this);
        mediaBuffer->buffer().swap(payload);
        if (!mediaBuffer->open(QIODevice::ReadOnly)) {
            delete mediaBuffer;
            reportNetworkError();
            return;
        }

        m_mediaBuffer = mediaBuffer;
        m_mediaPlayer->setMedia(QMediaContent(rUrl), mediaBuffer);
        startPlayback();
    });
}

void MediaPlayerPrivateQt::reportNetworkError()
{
    const MediaPlayer::NetworkState oldNetworkState = m_networkState;
    const MediaPlayer::ReadyState oldReadyState = m_readyState;
    m_networkState = MediaPlayer::NetworkError;
    m_readyState = MediaPlayer::HaveNothing;
    if (m_readyState != oldReadyState)
        m_webCorePlayer->readyStateChanged();
    if (m_networkState != oldNetworkState)
        m_webCorePlayer->networkStateChanged();
}

void MediaPlayerPrivateQt::startPlayback()
{
    // Set the current volume and mute status
    // We get these from the element, rather than the player, in case we have
    // transitioned from a media engine which doesn't support muting, to a media
    // engine which does.
    m_mediaPlayer->setMuted(m_webCorePlayer->muted());
    m_mediaPlayer->setVolume(static_cast<int>(m_webCorePlayer->volume() * 100.0));

    // Don't send PlaybackChanged notification for pre-roll.
    m_suppressNextPlaybackChanged = true;

    // Setting a media source will start loading the media, but we need
    // to pre-roll as well to get video size-hints and buffer-status
    if (m_webCorePlayer->paused())
        m_prerolling = true;
    m_mediaPlayer->play();
}

void MediaPlayerPrivateQt::resumeLoad()
{
    m_delayingLoad = false;

    if (!m_mediaUrl.isNull())
        commitLoad(m_mediaUrl);
}

void MediaPlayerPrivateQt::cancelLoad()
{
    m_mediaPlayer->setMedia(QMediaContent());
    updateStates();
}

void MediaPlayerPrivateQt::prepareToPlay()
{
    if (m_mediaPlayer->media().isNull() || m_delayingLoad)
        resumeLoad();
}

void MediaPlayerPrivateQt::play()
{
    m_prerolling = false;
    if (m_mediaPlayer->state() != QMediaPlayer::PlayingState)
        m_mediaPlayer->play();
}

void MediaPlayerPrivateQt::pause()
{
    if (m_mediaPlayer->state() == QMediaPlayer::PlayingState)
        m_mediaPlayer->pause();
}

bool MediaPlayerPrivateQt::paused() const
{
    return (m_prerolling || m_mediaPlayer->state() != QMediaPlayer::PlayingState);
}

void MediaPlayerPrivateQt::seek(float position)
{
    // The element is already in its seeking state by the time we get here and stays there until
    // timeChanged() reports back. A quiet return leaves it waiting forever, and after a few
    // scrubs playback stops being reported at all - so every path below has to notify.
    if (m_mediaPlayer->mediaStatus() == QMediaPlayer::NoMedia
        || m_mediaPlayer->mediaStatus() == QMediaPlayer::UnknownMediaStatus) {
        m_isSeeking = false;
        m_webCorePlayer->timeChanged();
        return;
    }

    // Neither isSeekable() nor availablePlaybackRanges() is consulted any more, and both used to
    // return quietly. The backend reports media as not seekable while it sits stopped at the end
    // of a clip - exactly when a viewer clicks the progress bar to watch it again - and seeking
    // past the buffer is ordinary scrubbing, which the backend satisfies by fetching what it
    // needs. Attempt the seek and let the backend answer.
    if (!m_isSeeking)
        m_resumePlaybackAfterSeek = m_mediaPlayer->state() == QMediaPlayer::PlayingState;
    m_isSeeking = true;
    const unsigned generation = ++m_seekGeneration;
    m_mediaPlayer->setPosition(static_cast<qint64>(position * 1000));

    // positionChanged() is the only signal that ends a seek, and the backend does not always
    // send one: a seek landing where the clip already sits, or rapid back-and-forth scrubs that
    // coalesce, produce none. Without a fallback m_isSeeking stays set and the element waits for
    // a timeChanged() that never comes. The generation check stops a stale watchdog from ending
    // a newer seek.
    QTimer::singleShot(1500, this, [this, generation]() {
        if (m_isSeeking && m_seekGeneration == generation)
            finishSeek();
    });
}

void MediaPlayerPrivateQt::finishSeek()
{
    const bool resumePlayback = m_resumePlaybackAfterSeek;
    m_resumePlaybackAfterSeek = false;

    // The Windows backend can stay in PlayingState with its presentation clock stalled after a
    // backward seek, so restart it even when the state says it is already playing. m_isSeeking
    // stays set through both calls, keeping those transitions invisible to the element.
    if (resumePlayback) {
        if (m_mediaPlayer->state() == QMediaPlayer::PlayingState)
            m_mediaPlayer->pause();
        m_mediaPlayer->play();
    }

    m_isSeeking = false;

    // timeChanged() completes WebCore's seek and schedules its own play-state reconciliation.
    m_webCorePlayer->timeChanged();
}

bool MediaPlayerPrivateQt::seeking() const
{
    return m_isSeeking;
}

float MediaPlayerPrivateQt::duration() const
{
    if (m_readyState < MediaPlayer::HaveMetadata)
        return 0.0f;

    float duration = m_mediaPlayer->duration() / 1000.0f;

    // We are streaming
    if (duration <= 0.0f)
        duration = std::numeric_limits<float>::infinity();

    return duration;
}

float MediaPlayerPrivateQt::currentTime() const
{
    return m_mediaPlayer->position() / 1000.0f;
}

std::unique_ptr<PlatformTimeRanges> MediaPlayerPrivateQt::buffered() const
{
    auto buffered = std::make_unique<PlatformTimeRanges>();

    // Media served from the application's own scheme is fetched whole before playback starts,
    // and for such a QIODevice source the backend reports no playback ranges at all. Left at
    // that, the element believes nothing is buffered and never starts playing - metadata
    // arrives, and then nothing. Report what is genuinely there instead.
    if (!m_mediaBuffer.isNull()) {
        const qint64 duration = m_mediaPlayer->duration();
        if (duration > 0) {
            buffered->add(MediaTime::zeroTime(),
                          MediaTime::createWithFloat(static_cast<float>(duration) / 1000.0f));
        }
        return buffered;
    }

    if (!m_mediaPlayerControl)
        return buffered;

    QMediaTimeRange playbackRanges = m_mediaPlayerControl->availablePlaybackRanges();

    foreach (const QMediaTimeInterval interval, playbackRanges.intervals()) {
        float rangeMin = static_cast<float>(interval.start()) / 1000.0f;
        float rangeMax = static_cast<float>(interval.end()) / 1000.0f;
        buffered->add(MediaTime::createWithFloat(rangeMin),
                      MediaTime::createWithFloat(rangeMax));
    }

    return buffered;
}

float MediaPlayerPrivateQt::maxTimeSeekable() const
{
    // Same reasoning as buffered(): the whole asset is in memory, so all of it is seekable,
    // whatever the backend reports for a QIODevice source.
    if (!m_mediaBuffer.isNull())
        return static_cast<float>(m_mediaPlayer->duration()) / 1000.0f;

    if (!m_mediaPlayerControl)
        return 0;

    return static_cast<float>(m_mediaPlayerControl->availablePlaybackRanges().latestTime()) / 1000.0f;
}

bool MediaPlayerPrivateQt::didLoadingProgress() const
{
    unsigned bytesLoaded = 0;
    QLatin1String bytesLoadedKey("bytes-loaded");
    if (m_mediaPlayer->availableMetaData().contains(bytesLoadedKey))
        bytesLoaded = m_mediaPlayer->metaData(bytesLoadedKey).toInt();
    else
        bytesLoaded = m_mediaPlayer->bufferStatus();
    bool didLoadingProgress = bytesLoaded != m_bytesLoadedAtLastDidLoadingProgress;
    m_bytesLoadedAtLastDidLoadingProgress = bytesLoaded;
    return didLoadingProgress;
}

unsigned long long MediaPlayerPrivateQt::totalBytes() const
{
    if (m_mediaPlayer->availableMetaData().contains(QMediaMetaData::Size))
        return m_mediaPlayer->metaData(QMediaMetaData::Size).toInt();

    return 100;
}

void MediaPlayerPrivateQt::setPreload(MediaPlayer::Preload preload)
{
    m_preload = preload;
    if (m_delayingLoad && m_preload != MediaPlayer::None)
        resumeLoad();
}

void MediaPlayerPrivateQt::setRate(float rate)
{
    m_mediaPlayer->setPlaybackRate(rate);
}

void MediaPlayerPrivateQt::setVolume(float volume)
{
    m_mediaPlayer->setVolume(static_cast<int>(volume * 100.0));
}

void MediaPlayerPrivateQt::setMuted(bool muted)
{
    m_mediaPlayer->setMuted(muted);
}

MediaPlayer::NetworkState MediaPlayerPrivateQt::networkState() const
{
    return m_networkState;
}

MediaPlayer::ReadyState MediaPlayerPrivateQt::readyState() const
{
    return m_readyState;
}

void MediaPlayerPrivateQt::setVisible(bool)
{
}

void MediaPlayerPrivateQt::mediaStatusChanged(QMediaPlayer::MediaStatus status)
{
    // Pre-roll done
    if (m_prerolling && (status == QMediaPlayer::BufferingMedia || status == QMediaPlayer::BufferedMedia)) {
        // Don't send PlaybackChanged notification for pre-roll.
        m_suppressNextPlaybackChanged = true;
        m_prerolling = false;
        m_mediaPlayer->pause();
    }

    updateStates();
}

void MediaPlayerPrivateQt::handleError(QMediaPlayer::Error)
{
    updateStates();
}

void MediaPlayerPrivateQt::stateChanged(QMediaPlayer::State)
{
    if (!m_suppressNextPlaybackChanged)
        m_webCorePlayer->playbackStateChanged();
    else
        m_suppressNextPlaybackChanged = false;
}

void MediaPlayerPrivateQt::surfaceFormatChanged(const QVideoSurfaceFormat& format)
{
    QSize size = format.sizeHint();
    LOG(Media, "MediaPlayerPrivateQt::naturalSizeChanged(%dx%d)",
            size.width(), size.height());

    if (!size.isValid())
        return;

    IntSize webCoreSize = size;
    if (webCoreSize == m_naturalSize)
        return;

    m_naturalSize = webCoreSize;
    m_webCorePlayer->sizeChanged();
}

void MediaPlayerPrivateQt::positionChanged(qint64)
{
    // Only propagate this event if we are seeking. finishSeek() rather than clearing the flag
    // here, so the signal and the watchdog end a seek by the same path.
    if (m_isSeeking)
        finishSeek();
}

void MediaPlayerPrivateQt::bufferStatusChanged(int)
{
    notImplemented();
}

void MediaPlayerPrivateQt::durationChanged(qint64)
{
    m_webCorePlayer->durationChanged();
}

void MediaPlayerPrivateQt::volumeChanged(int volume)
{
    m_webCorePlayer->volumeChanged(static_cast<float>(volume) / 100.0);
}

void MediaPlayerPrivateQt::mutedChanged(bool muted)
{
    m_webCorePlayer->muteChanged(muted);
}

void MediaPlayerPrivateQt::updateStates()
{
    // Store the old states so that we can detect a change and raise change events
    MediaPlayer::NetworkState oldNetworkState = m_networkState;
    MediaPlayer::ReadyState oldReadyState = m_readyState;

    QMediaPlayer::MediaStatus currentStatus = m_mediaPlayer->mediaStatus();
    QMediaPlayer::Error currentError = m_mediaPlayer->error();

    if (currentError != QMediaPlayer::NoError) {
        m_readyState = MediaPlayer::HaveNothing;
        if (currentError == QMediaPlayer::FormatError || currentError == QMediaPlayer::ResourceError)
            m_networkState = MediaPlayer::FormatError;
        else
            m_networkState = MediaPlayer::NetworkError;
    } else if (currentStatus == QMediaPlayer::UnknownMediaStatus
               || currentStatus == QMediaPlayer::NoMedia) {
        m_networkState = MediaPlayer::Idle;
        m_readyState = MediaPlayer::HaveNothing;
    } else if (currentStatus == QMediaPlayer::LoadingMedia) {
        m_networkState = MediaPlayer::Loading;
        m_readyState = MediaPlayer::HaveNothing;
    } else if (currentStatus == QMediaPlayer::LoadedMedia) {
        m_networkState = MediaPlayer::Loading;
        m_readyState = MediaPlayer::HaveMetadata;
    } else if (currentStatus == QMediaPlayer::BufferingMedia) {
        m_networkState = MediaPlayer::Loading;
        m_readyState = MediaPlayer::HaveFutureData;
    } else if (currentStatus == QMediaPlayer::StalledMedia) {
        m_networkState = MediaPlayer::Loading;
        m_readyState = MediaPlayer::HaveCurrentData;
    } else if (currentStatus == QMediaPlayer::BufferedMedia
               || currentStatus == QMediaPlayer::EndOfMedia) {
        m_networkState = MediaPlayer::Loaded;
        m_readyState = MediaPlayer::HaveEnoughData;
    } else if (currentStatus == QMediaPlayer::InvalidMedia) {
        m_networkState = MediaPlayer::FormatError;
        m_readyState = MediaPlayer::HaveNothing;
    }

    // Log the state changes and raise the state change events
    // NB: The readyStateChanged event must come before the networkStateChanged event.
    // Breaking this invariant will cause the resource selection algorithm for multiple
    // sources to fail.
    if (m_readyState != oldReadyState)
        m_webCorePlayer->readyStateChanged();

    if (m_networkState != oldNetworkState)
        m_webCorePlayer->networkStateChanged();
}

void MediaPlayerPrivateQt::setSize(const IntSize& size)
{
    LOG(Media, "MediaPlayerPrivateQt::setSize(%dx%d)",
            size.width(), size.height());

    if (size == m_currentSize)
        return;

    m_currentSize = size;
}

FloatSize MediaPlayerPrivateQt::naturalSize() const
{
    if (!hasVideo() ||  m_readyState < MediaPlayer::HaveMetadata) {
        LOG(Media, "MediaPlayerPrivateQt::naturalSize() -> 0x0 (!hasVideo || !haveMetaData)");
        return IntSize();
    }

    LOG(Media, "MediaPlayerPrivateQt::naturalSize() -> %dx%d (m_naturalSize)",
            m_naturalSize.width(), m_naturalSize.height());

    return m_naturalSize;
}

void MediaPlayerPrivateQt::removeVideoItem()
{
    m_mediaPlayer->setVideoOutput(static_cast<QAbstractVideoSurface*>(0));
}

void MediaPlayerPrivateQt::restoreVideoItem()
{
    m_mediaPlayer->setVideoOutput(this);
}

// Begin QAbstractVideoSurface implementation.

bool MediaPlayerPrivateQt::start(const QVideoSurfaceFormat& format)
{
    m_currentVideoFrame = QVideoFrame();
    m_frameFormat = format;

    // If the pixel format is not supported by QImage, then we return false here and the QtMultimedia back-end
    // will re-negotiate and call us again with a better format.
    if (QVideoFrame::imageFormatFromPixelFormat(m_frameFormat.pixelFormat()) == QImage::Format_Invalid)
        return false;

    return QAbstractVideoSurface::start(format);
}

QList<QVideoFrame::PixelFormat> MediaPlayerPrivateQt::supportedPixelFormats(QAbstractVideoBuffer::HandleType handleType) const
{
    QList<QVideoFrame::PixelFormat> formats;
    switch (handleType) {
    case QAbstractVideoBuffer::QPixmapHandle:
    case QAbstractVideoBuffer::NoHandle:
        formats << QVideoFrame::Format_RGB32 << QVideoFrame::Format_ARGB32 << QVideoFrame::Format_RGB565;
        break;
    default: break;
    }
    return formats;
}

bool MediaPlayerPrivateQt::present(const QVideoFrame& frame)
{
    m_currentVideoFrame = frame;
    m_webCorePlayer->repaint();
    return true;
}

// End QAbstractVideoSurface implementation.

void MediaPlayerPrivateQt::paint(GraphicsContext& context, const FloatRect& rect)
{
    if (m_composited)
        return;
    paintCurrentFrameInContext(context, rect);
}

void MediaPlayerPrivateQt::paintCurrentFrameInContext(GraphicsContext& context, const FloatRect& rect)
{
    if (context.paintingDisabled())
        return;

    if (!m_currentVideoFrame.isValid())
        return;

    QPainter* painter = context.platformContext();

    if (m_currentVideoFrame.handleType() == QAbstractVideoBuffer::QPixmapHandle) {
        painter->drawPixmap(QRectF(rect), m_currentVideoFrame.handle().value<QPixmap>(), QRectF(0, 0, rect.width(), rect.height()));
    } else if (m_currentVideoFrame.map(QAbstractVideoBuffer::ReadOnly)) {
        QImage image(m_currentVideoFrame.bits(),
                     m_frameFormat.frameSize().width(),
                     m_frameFormat.frameSize().height(),
                     m_currentVideoFrame.bytesPerLine(),
                     QVideoFrame::imageFormatFromPixelFormat(m_frameFormat.pixelFormat()));
        const QRectF target = rect;

        if (m_frameFormat.scanLineDirection() == QVideoSurfaceFormat::BottomToTop) {
            const QTransform oldTransform = painter->transform();
            painter->scale(1, -1);
            painter->translate(0, -target.bottom());
            painter->drawImage(QRectF(target.x(), 0, target.width(), target.height()), image);
            painter->setTransform(oldTransform);
        } else {
            painter->drawImage(target, image);
        }

        m_currentVideoFrame.unmap();
    }
}

void MediaPlayerPrivateQt::paintToTextureMapper(TextureMapper& textureMapper, const FloatRect& targetRect, const TransformationMatrix& matrix, float opacity)
{
}

} // namespace WebCore

#include "moc_MediaPlayerPrivateQt.cpp"
