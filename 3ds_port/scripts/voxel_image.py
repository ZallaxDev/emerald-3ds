"""Pillow pixel access shared by the voxel generators and verifier."""


def image_pixels(image):
    """Return every pixel in row order, including repeats and all channels."""
    flatten = getattr(image, "get_flattened_data", None)
    if flatten is not None:
        return flatten()
    # Older supported Pillow versions only provide getdata.
    return tuple(image.getdata())

# Alpha threshold and RGB channel extraction are done by Pillow's C core
# instead of calling getpixel() for each Python-level (x, y) coordinate.
_ALPHA_OPAQUE_128 = [0] * 128 + [255] * 128


def opaque_bottom(image):
    """Exclusive bottom row of alpha >= 128, as the old max(y) scan returned.

    Preserve the former ValueError for an image with no qualifying pixels.
    """
    box = image.getchannel("A").point(_ALPHA_OPAQUE_128).getbbox()
    if box is None:
        raise ValueError("max() arg is an empty sequence")
    return box[3]


def rgb_palette(image):
    """Distinct first-three-channel colours, ignoring alpha as before."""
    return {pixel[:3] for pixel in image_pixels(image)}
