// SPDX-License-Identifier: Apache-2.0
#include "update_check.h"

#include <stdexcept>

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "groomlab/semver.h"

namespace groomlab::app {

int compareVersions(const QString& a, const QString& b) {
    try {
        return groomlab::compareVersions(a.toStdString(), b.toStdString());
    } catch (const std::invalid_argument&) {
        return 0;  // not versions: nothing newer
    }
}

UpdateInfo parseLatest(const QByteArray& json, const QString& current) {
    UpdateInfo info;
    info.current = current;
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    info.latest = o.value(QStringLiteral("version")).toString();
    info.url = o.value(QStringLiteral("url")).toString();
    info.notes = o.value(QStringLiteral("notes")).toString().trimmed();
    if (info.latest.isEmpty()) {
        info.message = QStringLiteral("The update feed named no version");
        return info;
    }
    info.ok = true;
    info.newer = compareVersions(info.latest, current) > 0;
    info.message = info.newer ? QStringLiteral("GroomLab %1 is available (this is %2)").arg(info.latest, current)
                              : QStringLiteral("GroomLab %1 is the latest version").arg(current);
    return info;
}

UpdateChecker::UpdateChecker(QObject* parent) : QObject(parent) {}

QString UpdateChecker::feedUrl() {
#ifdef Q_OS_MACOS
    return QStringLiteral("https://downloads.getgroomlab.com/app/latest-macos.json");  // (tools/publish_app.py)
#else
    return QStringLiteral("https://downloads.getgroomlab.com/app/latest.json");
#endif
}

bool UpdateChecker::updatesEnabled() { return qEnvironmentVariableIntValue("GROOMLAB_NO_UPDATES") == 0; }

void UpdateChecker::check(const QString& current, Done done) {
    if (!updatesEnabled()) {
        UpdateInfo off;
        off.current = current;
        off.message = QStringLiteral("Update checks are off (GROOMLAB_NO_UPDATES)");
        done(off);
        return;
    }
    if (!network_) network_ = new QNetworkAccessManager(this);
    QNetworkRequest request{QUrl(feedUrl())};
    // A named client: Cloudflare turns away some default User-Agents.
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("GroomLab-App/%1").arg(current).toUtf8());
    request.setTransferTimeout(15000);
    QNetworkReply* reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, current, done = std::move(done)] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            UpdateInfo failed;
            failed.current = current;
            failed.message = QStringLiteral("Couldn't reach the update feed: %1").arg(reply->errorString());
            done(failed);
            return;
        }
        done(parseLatest(reply->readAll(), current));
    });
}

}  // namespace groomlab::app
