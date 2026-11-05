# cat-money · Go 服务端

本分支为 `components/server`。**Go 服务端相关文件统一放在 [`server/`](server/)**，包括 Go 模块、全部源码、浏览器面板、测试和详细使用文档；仓库根目录只保留项目入口、通用忽略规则与 CI 配置。

```text
README.md                 本分支入口
.gitignore                通用本地配置与缓存忽略规则
.github/workflows/         自动检查
server/
├── README.md              启动、配置、接口、测试与后续安全计划
├── go.mod / go.sum        Go 模块与依赖锁定
├── *.go                   服务端源码与 Go 测试
├── web/index.html         编译进二进制的网页
└── tests/                 固件协议兼容测试、网页测试与测试夹具
```

## 快速启动

需要 Go 1.22 或更新版本。请先进入 `server/`，再执行 Go 命令：

```bash
git clone --branch components/server https://github.com/zimu2025-001/cat-money.git
cd cat-money/server
go build -o esp32-proxy .
./esp32-proxy
```

Windows 使用 `go build -o esp32-proxy.exe .`，然后运行 `./esp32-proxy.exe`。浏览器打开 <http://localhost:8080/>。网页已内嵌，构建后的二进制可以单独复制到其他目录运行。

默认 HTTP / WebSocket 端口为 `8080`，摄像头 UDP 端口为 `5000`。本次仅整理目录，现有接口、功能和默认参数保持不变。**当前服务没有完整认证与加密，仅用于可信局域网或隔离测试环境，不要直接暴露到公网。**

详细配置、设备目标 IP、协议说明和后续安全计划见 [`server/README.md`](server/README.md)。已有工作区更新后，也需要先 `cd server` 再构建或测试。

## 本地检查

从仓库根目录执行：

```bash
cd server
go test -race -count=1 ./...
go vet ./...
go build -o esp32-proxy .
python -m unittest discover -s tests -v
node --test tests/test_camera_viewer.js
```

协议测试另需 Python 3.10+ 与 C 编译器，网页测试需 Node.js 18+；它们都不是服务运行依赖。GitHub Actions 从同一 `server/` 目录运行检查，并交叉编译 Linux、Windows、macOS 的 amd64 / arm64 二进制。

## 三端分支

- [`components/server`](https://github.com/zimu2025-001/cat-money/tree/components/server)：本分支，Go 服务端位于 `server/`。
- [`components/camera`](https://github.com/zimu2025-001/cat-money/tree/components/camera)：ESP32-S3 / OV2640 摄像头固件。
- [`components/controller`](https://github.com/zimu2025-001/cat-money/tree/components/controller)：小智主控固件。

各分支只包含对应端的代码。`main`、`dev/cc-fps` 和另外两个组件分支不受此次目录整理影响。不要把单端分支直接整分支合回完整项目分支，以免带入删除其他组件的变更。
