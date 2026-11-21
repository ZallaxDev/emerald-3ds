"""Pillow pixel access shared by the voxel generators and verifier."""


def image_pixels(image):
    """Return every pixel in row order, including repeats and all channels."""
    flatten = getattr(image, "get_flattened_data", None)
    if flatten is not None:
        return flatten()
    # Older supported Pillow versions only provide getdata.
    return tuple(image.getdata())
