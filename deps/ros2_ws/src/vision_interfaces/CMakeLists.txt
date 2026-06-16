cmake_minimum_required(VERSION 3.16)
project(vision_interfaces)

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)

set(SRV_FILES
  srv/CameraGetConfig.srv
  srv/CameraSetExposure.srv
  srv/CameraSetGain.srv
  srv/CameraSetTriggerMode.srv
  srv/CameraSoftTrigger.srv
  srv/DetectorGetConfig.srv
  srv/DetectorGetResult.srv
  srv/DetectorOnOff.srv
  srv/DetectorReloadTemplate.srv
  srv/DetectorSetGradThreshold.srv
  srv/DetectorSetSegmentMode.srv
  srv/DetectorSetThreshold.srv
  srv/DetectorSetVThreshold.srv
  srv/CommGetConfig.srv
  srv/CommGetStatus.srv
  srv/CommSetConfig.srv
)

rosidl_generate_interfaces(${PROJECT_NAME}
  ${SRV_FILES}
)

# 安装生成的共享库到安装目录，供运行时加载
install(TARGETS
  ${PROJECT_NAME}__rosidl_typesupport_cpp
  ${PROJECT_NAME}__rosidl_typesupport_c
  ${PROJECT_NAME}__rosidl_generator_c
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)

ament_package()
