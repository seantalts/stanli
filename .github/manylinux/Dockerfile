# Keep the architecture in the build workflow: PyPA publishes separate
# architecture-qualified images rather than one multi-architecture tag.
ARG MANYLINUX_IMAGE=quay.io/pypa/manylinux_2_28_x86_64
FROM ${MANYLINUX_IMAGE}

# This label links the GHCR package to the repository before its first push,
# which gives this repository's GITHUB_TOKEN pull access to the private image.
LABEL org.opencontainers.image.source="https://github.com/seantalts/stanli"

# These are build inputs, not workflow setup: putting them in one immutable
# image removes two repository-metadata refreshes from every wheel build.
RUN for attempt in 1 2 3 4 5; do \
        dnf install -y epel-release \
        && dnf install -y \
            unzip which diffutils patch rsync glibc-static ccache time git \
        && break; \
        [ "$attempt" -lt 5 ] || exit 1; \
        echo "dnf attempt $attempt failed, retrying"; \
        dnf clean all; \
        sleep $((attempt * 15)); \
    done \
    && dnf clean all \
    && rm -rf /var/cache/dnf

# The upstream manylinux image deliberately keeps its interpreters out of
# PATH. The wheel build uses 3.12 explicitly on every manylinux architecture.
ENV PATH="/opt/python/cp312-cp312/bin:${PATH}"
