#!/bin/bash
trap : SIGTERM SIGINT

function echoUsage()
{
    echo -e "Usage: ./replicate.sh [FLAG] \n\
            \t -s covins server [COMM_CONFIG SERVER_CONFIG] \n\
            \t -o ros orbslam client [COMM_CONFIG ROS_LAUNCHFILE DATASET]\n\
            \t -t terminal [DATASET]\n\
            \t -p play agent [DATASET AGENT_NUMBER]\n\
            \t -c run roscore \n\
            \t -v run rviz \n\
            \t -h help" >&2
}

function absPath()
{
    # generate absolute path from relative path
    # $1     : relative filename
    # return : absolute path
    if [ -d "$1" ]; then
        # dir
        (cd "$1"; pwd)
    elif [ -f "$1" ]; then
        # file
        if [[ $1 = /* ]]; then            \t -c run roscore \n\
            echo "$1"
        elif [[ $1 == */* ]]; then
            echo "$(cd "${1%/*}"; pwd)/${1##*/}"
        else
            echo "$(pwd)/$1"
        fi
    fi
}

function relativePath()
{
    # both $1 and $2 are absolute paths beginning with /
    # returns relative path to $2/$target from $1/$source
    source=$1
    target=$2

    common_part=$source # for now
    result="" # for now

    while [[ "${target#$common_part}" == "${target}" ]]; do
        # no match, means that candidate common part is not correct
        # go up one level (reduce common part)
        common_part="$(dirname $common_part)"
        # and record that we went back, with correct / handling
        if [[ -z $result ]]; then
            result=".."
        else
            result="../$result"
        fi
    done

    if [[ $common_part == "/" ]]; then
        # special case for root (no common path)
        result="$result/"
    fi


    # since we now have identified the common part,
    # compute the non-common part
    forward_part="${target#$common_part}"

    # and now stick all parts together
    if [[ -n $result ]] && [[ -n $forward_part ]]; then
        result="$result$forward_part"
    elif [[ -n $forward_part ]]; then
        # extra slash removal
        result="${forward_part:1}"
    fi

    echo $result
}

if [ "$#" -lt 1 ]; then
  echoUsage
  exit 1
fi

SERVER=0
ROS_CLIENT=0
ROS_CORE=0
RVIZ=0
PLAYER=0

while getopts "hsocvpt" opt; do
    case "$opt" in
        h)
            echoUsage
            exit 0
            ;;
        s)  SERVER=1
            ;;
        o)  ROS_CLIENT=1
            ;;
        c)  ROS_CORE=1
            ;;
        v)  RVIZ=1
            ;;
        p)  PLAYER=1
            ;;
        t)  ;;
        *)
            echoUsage
            exit 1
            ;;
    esac
done

CATKIN_WS=/root/covins_ws
ROS_IP=127.0.0.102
if [ $SERVER -eq 1 ]; then
        CONFIG_FILE_COMM=$(absPath ${*: -2:1})
        CONFIG_FILE_BACKEND=$(absPath ${*: -1})
        docker run \
        -it \
        --rm \
        --net=host \
        --volume "${CONFIG_FILE_COMM}:${CATKIN_WS}/src/covins/covins_comm/config/config_comm.yaml" \
        --volume "${CONFIG_FILE_BACKEND}:${CATKIN_WS}/src/covins/covins_backend/config/config_backend.yaml" \
        covins \
        /bin/bash -c \
                "cd ${CATKIN_WS}; \
                 source devel/setup.bash; \
                 rosrun covins_backend covins_backend_node"
elif [ $ROS_CLIENT -eq 1 ]; then
        CONFIG_FILE_COMM=$(absPath ${*: -3:2})
        LAUNCH_FILE=$(absPath ${*: -2:1})
        ARG_NUM=${*: -1}
        docker run \
        -it \
        --rm \
        --net=host \
        --env ROS_MASTER_URI=${ROS_MASTER_URI} \
        --volume "${CONFIG_FILE_COMM}:${CATKIN_WS}/src/covins/covins_comm/config/config_comm.yaml" \
        --volume "${LAUNCH_FILE}:${CATKIN_WS}/src/covins/orb_slam3/Examples/ROS/ORB_SLAM3/launch/launch_docker_ros_euroc.launch" \
        covins \
        /bin/bash -c \
                "cd ${CATKIN_WS}; \
                 source devel/setup.bash; \
                 roslaunch ORB_SLAM3 launch_docker_ros_euroc.launch ag_n:=${ARG_NUM}"
elif [ $ROS_CORE -eq 1 ]; then
        docker run \
        -it \
        --rm \
        --net=host \
        covins \
        /bin/bash -c \
                "cd ${CATKIN_WS}; \
                 roscore"
elif [ $RVIZ -eq 1 ]; then
        docker run \
        -it \
        --privileged \
        --rm \
        --net=host \
        --env DISPLAY=${DISPLAY} \
        --env QT_X11_NO_MITSHM=1 \
        --env XAUTHORITY=/tmp/.docker.xauth \
        --env ROS_IP=${ROS_IP} \
        --env ROS_MASTER_URI=${ROS_MASTER_URI} \
        --volume /tmp/.X11-unix:/tmp/.X11-unix:rw \
        --volume /tmp/.docker.xauth:/tmp/.docker.xauth \
        covins \
        /bin/bash -c \
                 "source devel/setup.bash; \
                 roslaunch src/covins/covins_backend/launch/tf.launch &
                 rviz -d /root/covins_ws/src/covins/covins_backend/config/covins.rviz"
elif [ $PLAYER -eq 1 ]; then
        CODE=$(absPath ${*: -3:2})
	    DATASET=$(absPath ${*: -2:1})
	    ARG_NUM=${*: -1}
        docker run \
        -it \
        --rm \
        --net=host \
        --env ROS_MASTER_URI=${ROS_MASTER_URI} \
        --volume "${CODE}:${CATKIN_WS}/code" \
        --volume "${DATASET}:${CATKIN_WS}/dataset" \
        --workdir "/root/covins_ws/code/player" \
        covins \
        /bin/bash -c "python main.py ${ARG_NUM}"
else
        CODE=$(absPath ${*: -2:1})
	    DATASET=$(absPath ${*: -1})
        docker run \
        -it \
        --rm \
        --net=host \
        --volume "${CODE}:${CATKIN_WS}/code" \
        --volume "${DATASET}:${CATKIN_WS}/dataset" \
        covins \
        /bin/bash
fi
