FROM ros:jazzy-ros-base@sha256:2589a8fba5257307857890173c069852c2abf913a0be7970f172478baecb09e4

LABEL autoruntime.local-only="true"

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       ros-jazzy-rmw-cyclonedds-cpp \
    && rm -rf /var/lib/apt/lists/*
