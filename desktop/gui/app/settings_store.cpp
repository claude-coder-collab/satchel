// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "settings_store.hpp"

#include <QSettings>

using satchel_gui::Overwrite;
using satchel_gui::Settings;

namespace
{

std::optional<std::filesystem::path> path_value(const QSettings& q, const char* key)
{
    const auto v = q.value(key).toString();
    if (v.isEmpty())
        return std::nullopt;
    return std::filesystem::path(v.toStdU16String());
}

QString path_text(const std::optional<std::filesystem::path>& p)
{
    return p ? QString::fromStdU16String(p->u16string()) : QString();
}

}

Settings load_settings()
{
    QSettings q;
    Settings s;
    s.start_in_last_mode = q.value("general/startInLastMode", s.start_in_last_mode).toBool();
    s.simple_output_folder = path_value(q, "general/simpleOutputFolder");
    s.flac = q.value("compression/flac", s.flac).toBool();
    s.flac_level = q.value("compression/flacLevel", s.flac_level).toInt();
    s.deflate_level = q.value("compression/deflateLevel", s.deflate_level).toInt();
    s.threads = q.value("compression/threads", s.threads).toInt();
    s.temp_folder = path_value(q, "compression/tempFolder");
    s.restore_audio = q.value("extraction/restoreAudio", s.restore_audio).toBool();
    s.include_readme = q.value("extraction/includeReadme", s.include_readme).toBool();
    s.overwrite = static_cast<Overwrite>(q.value("extraction/overwrite", static_cast<int>(s.overwrite)).toInt());
    s.verify_after_build = q.value("verification/afterBuild", s.verify_after_build).toBool();
    s.associate_zip = q.value("integration/associateZip", s.associate_zip).toBool();
    s.check_updates = q.value("updates/check", s.check_updates).toBool();
    return s;
}

void save_settings(const Settings& s)
{
    QSettings q;
    q.setValue("general/startInLastMode", s.start_in_last_mode);
    q.setValue("general/simpleOutputFolder", path_text(s.simple_output_folder));
    q.setValue("compression/flac", s.flac);
    q.setValue("compression/flacLevel", s.flac_level);
    q.setValue("compression/deflateLevel", s.deflate_level);
    q.setValue("compression/threads", s.threads);
    q.setValue("compression/tempFolder", path_text(s.temp_folder));
    q.setValue("extraction/restoreAudio", s.restore_audio);
    q.setValue("extraction/includeReadme", s.include_readme);
    q.setValue("extraction/overwrite", static_cast<int>(s.overwrite));
    q.setValue("verification/afterBuild", s.verify_after_build);
    q.setValue("integration/associateZip", s.associate_zip);
    q.setValue("updates/check", s.check_updates);
}

QString last_mode()
{
    return QSettings().value("general/lastMode", "simple").toString();
}

void set_last_mode(const QString& mode)
{
    QSettings().setValue("general/lastMode", mode);
}
