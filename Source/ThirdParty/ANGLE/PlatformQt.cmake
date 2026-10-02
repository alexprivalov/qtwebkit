# WebGL runs on Qt's desktop OpenGL, so the translator must emit GLSL as well as ESSL.
list(APPEND ANGLE_DEFINITIONS ANGLE_ENABLE_GLSL)
