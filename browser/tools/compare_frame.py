# Compare a frame the browser drew with a reference frame from native tmloader (--diagnostics screenshots).
#
#   python browser/tools/compare_frame.py <actual.png> <reference.png> [--threshold N] [--diff heat.png]
#
# Both images are blurred slightly, so texture filtering and one-pixel edge differences between WebGL2 and Direct3D 8
# do not count, then compared per pixel in luminance-weighted RGB. The score is the fraction of pixels that differ by
# more than --tolerance (default 8% of full scale): moving parts such as the mouse cursor or a counter touch well under
# 1% of the frame, a wrong or missing screen most of it. Exit 0 when the score is at most --threshold (default 0.02),
# 1 otherwise. The images are the user's game frames: keep them out of the repository.
import argparse
import sys

import numpy as np
from PIL import Image, ImageFilter

WEIGHTS = np.array([0.299, 0.587, 0.114], dtype=np.float32)


def load(path, size=None):
    image = Image.open(path).convert('RGB')
    if size and image.size != size:
        image = image.resize(size, Image.Resampling.BILINEAR)
    return np.asarray(image.filter(ImageFilter.GaussianBlur(1.0)), dtype=np.float32) / 255.0


def score(actual, reference, tolerance):
    """Fraction of pixels whose weighted colour difference exceeds tolerance, and the mean difference"""
    difference = np.abs(actual - reference) @ WEIGHTS / WEIGHTS.sum()
    return float((difference > tolerance).mean()), float(difference.mean()), difference


def main():
    parser = argparse.ArgumentParser(description='Compare a browser frame with a native reference frame')
    parser.add_argument('actual')
    parser.add_argument('reference')
    parser.add_argument('--threshold', type=float, default=0.02, help='largest fraction of differing pixels that matches')
    parser.add_argument('--tolerance', type=float, default=0.08, help='per-pixel difference that counts, 0..1')
    parser.add_argument('--diff', help='write a heat map of the differences here')
    args = parser.parse_args()
    reference_image = Image.open(args.reference)
    reference = load(args.reference)
    actual = load(args.actual, reference_image.size)
    fraction, mean, difference = score(actual, reference, args.tolerance)
    if args.diff:
        Image.fromarray(np.clip(difference * 4 * 255, 0, 255).astype(np.uint8)).save(args.diff)
    match = fraction <= args.threshold
    print('score %.4f (%.2f%% of pixels differ by more than %.0f%%; mean difference %.4f): %s'
          % (fraction, fraction * 100, args.tolerance * 100, mean, 'match' if match else 'DIFFERENT'))
    return 0 if match else 1


if __name__ == '__main__':
    sys.exit(main())
