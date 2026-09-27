"""Reconstruct the three native glove poses as smooth, coordinate-stable curves.

The paths are in the original 40x40 sprite-cell coordinates. No source pixel
contours are enlarged or automatically traced. Only this offline builder uses
Pillow/NumPy/OpenCV; the loader continues to see an ordinary replacement PNG.
"""

from __future__ import annotations

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

CELL_SIZE = 40
SOURCE_SIZE = (120, 40)
INK_BOUNDS = ((4, 4, 30, 36), (3, 6, 31, 35), (2, 9, 32, 33))

# Each path starts at the fingertip. Subsequent tuples hold the two controls
# and endpoint of a cubic Bezier segment. These are three distinct poses, not
# rotations of a shared icon: the native thumb/cuff silhouettes differ.
OUTLINES = (
    (
        (9.4, 4.1),
        (8.0, 4.0, 6.0, 7.8, 6.2, 9.9),
        (6.4, 11.9, 10.1, 14.1, 11.7, 15.9),
        (11.2, 17.0, 9.3, 17.5, 7.8, 17.6),
        (6.0, 18.2, 4.1, 19.8, 4.1, 21.1),
        (4.0, 23.1, 7.2, 24.7, 9.0, 26.2),
        (10.3, 28.1, 12.7, 30.6, 15.8, 31.7),
        (17.5, 32.3, 18.5, 35.7, 20.7, 35.8),
        (23.0, 35.1, 28.7, 29.4, 29.8, 27.5),
        (30.0, 26.2, 29.9, 24.1, 29.5, 23.5),
        (28.9, 22.4, 26.3, 21.6, 25.5, 20.5),
        (25.3, 19.2, 25.0, 18.1, 24.0, 17.0),
        (20.9, 13.7, 15.4, 9.6, 11.2, 5.3),
        (10.5, 4.7, 10.0, 4.1, 9.4, 4.1),
    ),
    (
        (6.3, 6.1),
        (5.2, 6.1, 3.0, 9.0, 3.1, 10.8),
        (3.5, 12.6, 6.2, 14.6, 8.8, 16.8),
        (8.9, 18.1, 5.5, 18.4, 4.3, 20.6),
        (3.8, 23.7, 6.0, 27.4, 8.0, 29.1),
        (9.4, 30.5, 11.3, 32.8, 13.3, 32.8),
        (15.1, 32.8, 16.1, 31.6, 17.4, 31.8),
        (18.9, 33.5, 21.2, 34.8, 23.4, 34.8),
        (25.1, 33.5, 28.3, 29.5, 29.6, 26.7),
        (29.5, 26.1, 28.9, 25.8, 29.2, 25.4),
        (30.8, 24.6, 30.9, 22.8, 30.8, 21.6),
        (30.2, 20.6, 26.6, 19.8, 24.6, 18.6),
        (22.7, 16.9, 21.5, 15.6, 19.2, 14.9),
        (15.5, 13.0, 10.9, 9.2, 7.5, 6.9),
        (7.0, 6.5, 6.6, 6.1, 6.3, 6.1),
    ),
    (
        (4.2, 9.1),
        (2.1, 9.8, 2.0, 11.8, 2.1, 13.6),
        (2.0, 15.6, 5.3, 17.0, 9.6, 19.2),
        (9.4, 20.4, 7.2, 22.0, 7.1, 24.2),
        (6.8, 27.3, 9.5, 30.4, 11.8, 31.7),
        (13.3, 31.7, 14.4, 32.8, 16.4, 32.8),
        (18.3, 32.9, 19.5, 32.0, 20.7, 31.9),
        (22.5, 32.2, 25.4, 33.0, 27.6, 32.7),
        (29.7, 31.2, 29.7, 28.0, 30.9, 25.8),
        (32.0, 24.3, 31.9, 20.9, 31.5, 19.4),
        (31.0, 18.0, 29.9, 17.2, 28.8, 17.4),
        (27.6, 17.7, 26.9, 17.9, 25.9, 17.4),
        (20.0, 13.8, 12.3, 11.9, 6.1, 9.4),
        (5.4, 9.1, 4.8, 9.0, 4.2, 9.1),
    ),
)

CREASES = (
    (
        ((11.8, 16.2), (10.9, 18.0, 10.8, 20.0, 11.5, 22.2)),
        ((9.4, 23.1), (9.5, 25.1, 11.0, 27.1, 12.5, 28.0)),
        ((16.2, 28.9), (18.3, 29.9, 20.1, 31.0, 21.2, 33.0)),
    ),
    (
        ((9.2, 17.0), (8.3, 19.4, 9.0, 22.4, 10.1, 24.1)),
        ((10.1, 25.1), (10.3, 27.0, 12.0, 29.0, 13.3, 30.2)),
        ((17.2, 28.6), (19.2, 29.6, 21.4, 30.0, 23.0, 31.8)),
    ),
    (
        ((10.0, 19.0), (9.5, 21.4, 10.1, 23.0, 11.7, 24.1)),
        ((11.6, 25.0), (12.2, 27.1, 13.8, 28.3, 15.0, 29.4)),
        ((20.0, 28.8), (22.8, 29.0, 25.0, 29.5, 26.3, 30.3)),
    ),
)


def _points(path, scale):
    """Flatten editable cubic curves to subpixel-accurate drawing coordinates."""
    start = np.asarray(path[0], dtype=float)
    result = [tuple(start * scale)]
    t = np.linspace(0, 1, 33)[1:, None]
    for segment in path[1:]:
        first, second, end = np.asarray(segment, dtype=float).reshape(3, 2)
        curve = (
            (1 - t) ** 3 * start
            + 3 * (1 - t) ** 2 * t * first
            + 3 * (1 - t) * t**2 * second
            + t**3 * end
        )
        result.extend(map(tuple, curve * scale))
        start = end
    return result


def _pose(index, scale):
    import cv2

    working_scale = scale * 4
    size = CELL_SIZE * working_scale
    mask = Image.new("L", (size, size))
    ImageDraw.Draw(mask).polygon(_points(OUTLINES[index], working_scale), fill=255)
    coverage = np.asarray(mask)
    distance = cv2.distanceTransform(coverage, cv2.DIST_L2, cv2.DIST_MASK_PRECISE)
    distance /= working_scale

    # A narrow charcoal contour and rounded white bevel match the glove's
    # original material. Lighting changes smoothly; it is not a pixel filter.
    bevel = np.clip((distance - 0.6) / 1.5, 0, 1)
    height = cv2.GaussianBlur(bevel, (0, 0), working_scale * 0.2)
    dx = cv2.Sobel(height, cv2.CV_32F, 1, 0) * working_scale / 8
    dy = cv2.Sobel(height, cv2.CV_32F, 0, 1) * working_scale / 8
    brightness = 170 + 79 * bevel + 38 * (dx + dy)
    rim = np.clip((distance - 0.15) / 0.6, 0, 1)
    brightness = 22 * (1 - rim) + np.clip(brightness, 105, 255) * rim

    folds = Image.new("L", (size, size))
    draw = ImageDraw.Draw(folds)
    for crease in CREASES[index]:
        draw.line(
            _points(crease, working_scale), fill=255, width=round(0.4 * working_scale)
        )
    folds = folds.filter(ImageFilter.GaussianBlur(working_scale * 0.32))
    brightness -= np.asarray(folds, dtype=np.float32) * 0.21
    rgb = np.clip(brightness, 0, 255).astype(np.uint8)
    image = Image.fromarray(np.dstack((rgb, rgb, rgb, coverage)))
    result = np.array(image.resize((CELL_SIZE * scale,) * 2, Image.Resampling.LANCZOS))
    # Lanczos can leave faint ringing beyond a shape. Keep even that coverage
    # inside the original ink rectangle so native UV gutters stay transparent.
    left, top, right, bottom = (value * scale for value in INK_BOUNDS[index])
    result[:top] = 0
    result[bottom:] = 0
    result[:, :left] = 0
    result[:, right:] = 0
    return Image.fromarray(result)


def vector_cursor(source, scale=4):
    """Return (RGBA atlas, audit) for either audited 120x40 native cursor PNG.

    Source pixels only verify the layout. All visible replacement geometry is
    defined above, so duplicate source atlases always receive identical output.
    """
    if not isinstance(scale, int) or isinstance(scale, bool) or scale < 1 or scale > 8:
        raise ValueError("Cursor scale must be an integer in 1..8")
    source = np.asarray(source)
    if source.shape != (SOURCE_SIZE[1], SOURCE_SIZE[0], 4) or source.dtype != np.uint8:
        raise ValueError("Expected the native 120x40 uint8 RGBA cursor atlas")
    canvas = Image.new("RGBA", (SOURCE_SIZE[0] * scale, SOURCE_SIZE[1] * scale))
    poses = []
    for index, bounds in enumerate(INK_BOUNDS):
        original = source[:, index * CELL_SIZE : (index + 1) * CELL_SIZE]
        ys, xs = np.where(original[:, :, 3] > 32)
        if not len(xs) or (xs.min(), ys.min(), xs.max() + 1, ys.max() + 1) != bounds:
            raise ValueError(f"Unexpected native cursor bounds in pose {index}")
        pose = _pose(index, scale)
        canvas.paste(pose, (index * CELL_SIZE * scale, 0))
        poses.append(
            {
                "pose": index,
                "source_bounds": list(bounds),
                "output_bounds": list(pose.getbbox()),
            }
        )
    return np.asarray(canvas), {
        "method": "hand-authored cubic glove contours and white bevel",
        "scale": scale,
        "poses": poses,
    }
