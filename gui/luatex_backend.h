#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <functional>
#include <memory>

namespace stemtex {

struct LuaBackendConfig {
  QString repoRoot;
  QString runtimeRoot;
  QString texmfRoot;
  QString enginePath;
  QString profileRoot;
  QString stateRoot;
  int timeoutMs = 90000;
};

struct LuaRenderResult {
  bool ok = false;
  QString outputPath;
  QString outputFormat;
  QString summary;
  QString error;
  qint64 workerPid = 0;
  int pageNumber = 0;
  int issueFlags = 0;
  bool recoverable = false;
  QString outcomeMessage;
};

// All operations except cancel(), ready(), and workerPid() belong to one
// background thread. Cancellation is terminal for this backend instance.
// Only one active GUI backend may own the resident converter DLLs at a time;
// serialize initialization/destruction with the XeTeX renderer lifecycle.
// Profiles declaring outputDrivers.svg=dvisvgm use separate resident PDF/SVG
// workers, with SVG started lazily. workerPid() reports the most recent output's
// worker. Profiles without that metadata retain one shared worker.
class LuaBackend {
 public:
  explicit LuaBackend(LuaBackendConfig config);
  ~LuaBackend();
  bool initialize(const std::function<void(const QString &)> &progress, QString *error);
  LuaRenderResult render(const QByteArray &snippet, double widthPt, double fontSizePt, bool svg);
  void cancel();
  bool ready() const;
  qint64 workerPid() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace stemtex
