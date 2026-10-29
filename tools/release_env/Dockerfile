# The environment a release is made in (tools/docker_release.py): devkitARM,
# libctru, citro2d and citro3d, the decomp's host tools, and the one Python
# and Pillow the voxel data is made with (upstream.lock [python]). The
# images come in as build arguments, from upstream.lock.
ARG PYTHON_IMAGE
ARG DEVKITARM_IMAGE
FROM ${PYTHON_IMAGE} AS python

FROM ${DEVKITARM_IMAGE}
ARG PILLOW
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ git libpng-dev pkg-config xz-utils \
        libbz2-1.0 libffi8 liblzma5 libncursesw6 libreadline8 libsqlite3-0 libssl3 zlib1g \
    && rm -rf /var/lib/apt/lists/*
# the Python image's interpreter, over the same Debian release
COPY --from=python /usr/local /usr/local
COPY wheels /wheels
RUN ldconfig \
    && (ls /wheels/*.whl >/dev/null 2>&1 \
        && python3 -m pip install --no-cache-dir --no-index /wheels/*.whl \
        || python3 -m pip install --no-cache-dir "pillow==${PILLOW}") \
    && python3 -c "import PIL, sys; assert PIL.__version__ == '${PILLOW}', PIL.__version__; print(sys.version)"
ENV DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
ENV PATH=/usr/local/bin:/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:$PATH
