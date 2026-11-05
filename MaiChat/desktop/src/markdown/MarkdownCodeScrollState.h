// Fenced-code scrolling is local to a host, never part of the shared layout
// cache. Body text and the parent view retain their normal vertical scrolling.
#pragma once

#include <QPoint>
#include "markdown/MarkdownLayout.h"

struct MarkdownCodeScrollState {
    QHash<int, qreal> offsets;
    int draggingBlock = -1;
    qreal pressX = 0;
    qreal pressOffset = 0;

    void clear() { offsets.clear(); draggingBlock = -1; }

    void synchronize(const MarkdownLayout& layout) {
        const auto& blocks = layout.codeBlocks();
        for (auto it = offsets.begin(); it != offsets.end();) {
            if (it.key() >= blocks.size()) it = offsets.erase(it);
            else { it.value() = qBound<qreal>(0, it.value(), blocks[it.key()].maximumOffset()); ++it; }
        }
        if (draggingBlock >= blocks.size()) draggingBlock = -1;
    }

    bool press(const MarkdownLayout& layout, const QPointF& point) {
        synchronize(layout);
        const auto& blocks = layout.codeBlocks();
        for (int index = 0; index < blocks.size(); ++index) {
            const auto& block = blocks[index];
            const QRectF track = block.scrollTrack();
            if (track.isEmpty() || !track.adjusted(0, -4 * block.zoom, 0, 4 * block.zoom).contains(point))
                continue;
            const QRectF thumb = block.scrollThumb(offsets.value(index));
            // Clicking the track brings that part of the source into view;
            // subsequent movement drags the same thumb without selecting text.
            if (!thumb.contains(QPointF(point.x(), thumb.center().y()))) {
                const qreal travel = track.width() - thumb.width();
                offsets[index] = travel <= 0 ? 0 : qBound<qreal>(0,
                    (point.x() - track.left() - thumb.width() / 2) / travel, 1) * block.maximumOffset();
            }
            draggingBlock = index;
            pressX = point.x();
            pressOffset = offsets.value(index);
            return true;
        }
        return false;
    }

    bool move(const MarkdownLayout& layout, const QPointF& point) {
        if (draggingBlock < 0) return false;
        synchronize(layout);
        if (draggingBlock < 0) return false;
        const auto& block = layout.codeBlocks()[draggingBlock];
        const qreal travel = block.scrollTrack().width() - block.scrollThumb(pressOffset).width();
        offsets[draggingBlock] = travel <= 0 ? 0 : qBound<qreal>(0,
            pressOffset + (point.x() - pressX) * block.maximumOffset() / travel, block.maximumOffset());
        return true;
    }

    bool release() {
        const bool active = draggingBlock >= 0;
        draggingBlock = -1;
        return active;
    }

    bool wheel(const MarkdownLayout& layout, const QPointF& point, const QPoint& pixels,
               const QPoint& angle, Qt::KeyboardModifiers modifiers) {
        // A plain vertical wheel belongs to the message list, even over code.
        const bool shifted = modifiers.testFlag(Qt::ShiftModifier);
        qreal delta = pixels.x();
        if (qFuzzyIsNull(delta) && shifted) delta = pixels.y();
        if (qFuzzyIsNull(delta)) {
            delta = angle.x();
            if (qFuzzyIsNull(delta) && shifted) delta = angle.y();
            delta /= 3; // One wheel notch moves a readable code-column segment.
        }
        if (qFuzzyIsNull(delta)) return false;
        synchronize(layout);
        const auto& blocks = layout.codeBlocks();
        for (int index = 0; index < blocks.size(); ++index) {
            const auto& block = blocks[index];
            if (!block.bounds.contains(point) || block.maximumOffset() <= 0) continue;
            offsets[index] = qBound<qreal>(0, offsets.value(index) - delta, block.maximumOffset());
            return true;
        }
        return false;
    }
};
