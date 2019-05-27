### build latest with 16.04
### after ros_gl
###
FROM yoheikakiuchi/ros_gl:16.04

## WORKDIR /userdir

SHELL ["/bin/bash", "-c"]

ENV CNOID_PARENT_SETUP /opt/ros/$ROS_DISTRO/setup.bash

RUN apt-get update -q -qq && apt-get install -q -qq -y \
    mesa-utils python-pip \
    ros-${ROS_DISTRO}-catkin python-wstools python-catkin-tools \
# choreonoid
    libyaml-dev ros-kinetic-openrtm-aist ros-kinetic-openrtm-aist-python libqt4-opengl-dev python3-yaml \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

### install choreonoid
ENV CNOID_BRANCH master

## choreonoid
WORKDIR /choreonoid_ws

RUN wstool init src

COPY . src/choreonoid

RUN wstool set -y -t src choreonoid_rosplugin       https://github.com/YoheiKakiuchi/choreonoid_rosplugin.git -v master_jsk --git && \
#    wstool set -y -t src choreonoid_rosplugin       https://github.com/s-nakaoka/choreonoid_rosplugin.git -v master_jsk --git && \
#    wstool set -y -t src choreonoid_ros_samples     https://github.com/s-nakaoka/choreonoid_ros_samples.git --git && \
#    wstool set -y -t src choreonoid_joy             https://github.com/s-nakaoka/choreonoid_joy.git --git && \
    wstool update -t src && \
    sed -i -e 's@OPENRTM_VERSION STREQUAL "1.1.2"@OPENRTM_VERSION STREQUAL "1.1.0"@' src/choreonoid/CMakeLists.txt && \
    sed -i -e 's@OPENRTM_VERSION VERSION_EQUAL "1.1.2"@OPENRTM_VERSION VERSION_EQUAL "1.1.0"@' src/choreonoid/CMakeLists.txt && \    
    sed -i -e 's@sudo apt-get -y install@apt-get install -q -qq -y@' src/choreonoid/misc/script/install-requisites-ubuntu-16.04.sh && \
    apt-get update -q -qq && \
    src/choreonoid/misc/script/install-requisites-ubuntu-16.04.sh \
    && rosdep install -q -y -r --from-paths src --ignore-src || echo 'Ignore rosdep error' \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /choreonoid_ws

RUN source ${CNOID_PARENT_SETUP} && \
    catkin build choreonoid --cmake-args -DENABLE_CORBA=ON -DBUILD_CORBA_PLUGIN=ON -DBUILD_OPENRTM_PLUGIN=ON -DBUILD_HELLO_WORLD_SAMPLE=ON -DBUILD_SPRING_MODEL_SAMPLE=ON -DUSE_BUILTIN_CAMERA_IMAGE_IDL=ON -DBUILD_WRS2018=ON -DUSE_PYTHON3=OFF

RUN source ${CNOID_PARENT_SETUP} && \
    if [ -e /choreonoid_ws/src/choreonoid/thirdparty/pybind11-2.2.4/include/pybind11 ]; then ln -sf /choreonoid_ws/src/choreonoid/thirdparty/pybind11-2.2.4/include/pybind11 devel/include/choreonoid-1.7; fi && \
    catkin build            --cmake-args -DENABLE_CORBA=ON -DBUILD_CORBA_PLUGIN=ON -DBUILD_OPENRTM_PLUGIN=ON -DBUILD_HELLO_WORLD_SAMPLE=ON -DBUILD_SPRING_MODEL_SAMPLE=ON -DUSE_BUILTIN_CAMERA_IMAGE_IDL=ON -DBUILD_WRS2018=ON -DUSE_PYTHON3=OFF && \
    catkin clean -b -l -y

### robotsimulation-docker
## ADD ./my_entrypoint.sh /
## ADD ./my_entryrc /
## ENTRYPOINT ["/my_entrypoint.sh"]
## CMD ["bash"]
