#pragma once

#include "hal/StackChanBoard.h"
#include "services/CameraService.h"
#include "ui/FaceRenderer.h"
#include <WebServer.h>

class WebControlService {
public:
  bool begin(StackChanBoard &board, FaceRenderer &face,
             CameraService &camera);
  void update();
  void stop();
  String address() const;
  bool accessPointMode() const { return _accessPointMode; }

private:
  WebServer _server{80};
  StackChanBoard *_board = nullptr;
  FaceRenderer *_face = nullptr;
  CameraService *_camera = nullptr;
  bool _accessPointMode = false;
  bool _available = false;

  bool connectNetwork();
  void configureRoutes();
  void sendState();
  void sendJsonError(int status, const char *message);
  void applyExpression(Expression expression);
};
