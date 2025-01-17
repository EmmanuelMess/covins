# This is an auto generated Dockerfile for ros:desktop-full
# generated from docker_images/create_ros_image.Dockerfile.em
FROM osrf/ros:melodic-desktop-bionic

ENV CATKIN_WS=/root/covins_ws

# fix keys see (https://github.com/googlecartographer/cartographer_ros/issues/1323)
RUN apt-key adv --keyserver 'hkp://keyserver.ubuntu.com:80' --recv-key 4B63CF8FDE49746E98FA01DDAD19BAB3CBF125EA
RUN apt-key del 421C365BD9FF1F717815A3895523BAEEB01FA116

# install ros packages
RUN apt-get update && apt-get install -y \
                      wget \
                      ros-melodic-desktop-full \
                      ros-melodic-serial \
					  ros-melodic-pcl-ros\
                      software-properties-common\
                      python3-catkin-tools \
                      python3-wstool \
                      vim \
                      sudo \
					  net-tools\
                      autoconf \
                      mesa-utils\
	        	      gedit \
                      gdb \
                      cgdb \
                      && rm -rf /var/lib/apt/lists/*

RUN apt-get update && apt-get upgrade -y

# -------------------------------------------
# Libraries for building COVINS
# -------------------------------------------
RUN apt-get update && apt-get install -y \
                      cmake \
					  doxygen \
                      libgoogle-glog-dev \
                      libatlas-base-dev \
                      libsuitesparse-dev \
					  libyaml-cpp-dev \
					  libvtk6-dev \
					  libv4l-dev \
                      libomp-dev \
                      libglew-dev

    
#--------------------------
# Add opencv
#--------------------------
RUN apt-get update && apt-get install -y \
                      python-pip \
                      python-tk \
                      && rm -rf /var/lib/apt/lists/*

RUN python -m pip install pip==20.3.4
RUN python -m pip install opencv-python==4.2.0.32 bagpy pathlib

#--------------------------
# BUILD COVINS
#--------------------------
COPY ./ $CATKIN_WS/src/covins
ARG NR_JOBS=
WORKDIR $CATKIN_WS
ENV TERM xterm
ENV PYTHONIOENCODING UTF-8

RUN catkin init && \
    catkin config \
      --extend /opt/ros/melodic \
      --merge-devel \
      --cmake-args -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
    chmod +x src/covins/install_file.sh && \
    ./src/covins/install_file.sh $NR_JOBS && \
    sed -i '/exec "$@"/i \
            source "${CATKIN_WS}/devel/setup.bash"' /ros_entrypoint.sh

#--------------------------
# BUILD COVINS ROS NODE
#--------------------------
WORKDIR $CATKIN_WS
RUN cd src && \
    git clone https://github.com/ros-perception/vision_opencv.git && \
    cd vision_opencv && \
    git checkout melodic && \
    cd cv_bridge && \
    sed -i '4d' CMakeLists.txt && \
    sed -i '3afind_package(catkin REQUIRED COMPONENTS rosconsole sensor_msgs opencv3_catkin)' CMakeLists.txt && \
    catkin build -j $NR_JOBS cv_bridge && \
    cd $CATKIN_WS/devel/lib && \
    ldd libcv_bridge.so | grep opencv_core && \
    catkin build -j $NR_JOBS ORB_SLAM3
    
