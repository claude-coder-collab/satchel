// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
import Foundation
import QuickLookUI
import SatchelC
import UniformTypeIdentifiers

struct PreviewError: Error, CustomStringConvertible {
    let description: String

    static func last() -> PreviewError {
        PreviewError(description: String(cString: zp_last_error()))
    }
}

/// Quick Look preview of zip archives and of FLAC files made by Satchel, rendered by the core's
/// zp_preview (metadata only, never audio).
final class PreviewProvider: QLPreviewProvider, QLPreviewingController {
    func providePreview(for request: QLFilePreviewRequest) async throws -> QLPreviewReply {
        let html = try Self.html(for: request.fileURL)
        let reply = QLPreviewReply(dataOfContentType: .html, contentSize: CGSize(width: 760, height: 560)) { reply in
            reply.stringEncoding = .utf8
            return Data(html.utf8)
        }
        reply.title = request.fileURL.lastPathComponent
        return reply
    }

    static func html(for url: URL) throws -> String {
        guard let stream = zp_stream_open_file(url.path) else {
            throw PreviewError.last()
        }
        defer { zp_stream_free(stream) }
        guard let text = zp_preview(stream, url.lastPathComponent, Int32(ZP_PREVIEW_HTML)) else {
            throw PreviewError.last()
        }
        defer { zp_free(text) }
        return String(cString: text)
    }
}
