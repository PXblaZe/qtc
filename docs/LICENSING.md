# Licensing

qtc and goose-qt are MIT-licensed. A program that uses them also uses Qt, so Qt's
licence terms apply to that program too. Which terms apply depends on two
things:

1. Which Qt modules your build of `libqtc` links.
2. Which Qt build, with which third-party libraries, you ship.

This page is a summary to help you choose a build. It is not legal advice. For a
product decision, read the licence texts and [Qt's licensing
pages](https://www.qt.io/licensing), or ask a lawyer.

## When the licences matter

The LGPL and the GPL impose obligations when you *distribute* a program, for
example when you give or sell binaries to others or ship devices that contain
them. They impose nothing on software you build and run only for yourself or
inside your organisation.

The GPL-3.0 is not the AGPL. Letting people use a program over a network does not
count as distributing it.

## The default build links only LGPL-3.0 Qt modules

Every Qt module that a default build binds is available under the LGPL-3.0 (or
the GPL-2.0/GPL-3.0, or a commercial licence). The SPDX headers of Qt 6.12 list
these modules:

| Modules |
|---|
| Core, Gui, Widgets, Network, Sql, PrintSupport, Xml, DBus, OpenGL, OpenGLWidgets |
| Svg, SvgWidgets, Qml, Quick, QuickWidgets, QuickControls2 |
| Multimedia, MultimediaWidgets, SpatialAudio, SerialPort, Positioning |
| WebSockets, WebChannel, UiTools |

The graphics layer also links Qt Shader Tools (to compile shaders at run time),
which Qt 6.12's SBOM lists as `LicenseRef-Qt-Commercial OR LGPL-3.0-only OR
GPL-2.0-only OR GPL-3.0-only`, like the modules above. It bundles glslang
(BSD-3-Clause, MIT-style Khronos, Apache-2.0, and a Bison-generated parser under
the GPL-3.0-or-later *with the Bison exception*, which lets the parser be used
under any terms) and SPIRV-Cross (Apache-2.0); their notices go with Qt's.
Qt's `qsb` command-line tool is GPL-3.0 with the Qt GPL exception: the shader
packages it writes are not covered by it. `-DQTC_SHADER_TOOLS=OFF` builds
without Shader Tools (shaders are then loaded as `.qsb` packages).

The LGPL-3.0 lets closed-source programs use these modules. In outline, when you
distribute such a program:

- **Link Qt dynamically.** `libqtc` always links Qt as shared libraries. Don't
  build Qt statically into a closed-source product.
- **Let users replace the Qt libraries with their own builds.** On consumer
  devices ("User Products" in LGPL-3.0 terms), give them the information needed
  to install a modified Qt.
- **Ship the LGPL-3.0 text and Qt's notices** with the program.
- **Release changes you make to Qt itself** under the LGPL. Your own program's
  source stays yours.

## The GPL-3.0-only modules are opt-in

These five modules are available under the GPL-3.0 only, or under a commercial
Qt licence:

| Module | What it provides |
|---|---|
| Charts | QChart, the series classes, QChartView |
| NetworkAuth | OAuth 1 and OAuth 2 |
| HttpServer | QHttpServer, QAbstractHttpServer |
| Mqtt | QMqttClient and related classes |
| Coap | QCoapClient and related classes |

A `libqtc` that links any of them puts every program using it under GPL-3.0
terms when distributed, unless you hold a commercial Qt licence. A default build
leaves them out, even when they are installed:

```sh
cmake -B build                       # LGPL-3.0 Qt modules only (the default)
cmake -B build -DQTC_GPL_MODULES=ON  # also Charts, NetworkAuth, HttpServer, Mqtt, Coap
```

Two more safeguards apply:

- If you name one of these modules in `QTC_MODULES` without
  `-DQTC_GPL_MODULES=ON`, configuring stops with an explanation.
- A build that includes them warns at configure time and lists them in
  `qtc_stats()` under `"gpl_only_modules"`. That list is empty for the default
  build, so a program can check which kind of `libqtc` it is running on.

From 1.2.0 on, every module beyond Qt Core, Gui and Widgets is bound by a
library of its own (`libqtc_Charts.so`, `libqtc_Mqtt.so` and so on, next to
`libqtc`), loaded the first time a program uses one of its classes. `libqtc`
itself then never links a GPL-3.0-only module: those modules are linked only by
their own `libqtc_<Module>` libraries, which a program that does not use them
never loads, and which a distribution can simply leave out (whether a program
that does load one is a combined work under the GPL is the same question as for
any plugin; the opt-in above still applies). `"gpl_only_modules"` in
`qtc_stats()` lists those of them whose library is present.

qtc 1.0.0 bound Qt Charts whenever it was installed. A default 1.0.0 build
therefore linked a GPL-3.0-only module. From 1.1.0 on, the default build does
not.

### Alternatives without the GPL

Programs that need these features without the GPL can use permissively licensed
C libraries directly. They work alongside qtc from C, and from Goose through
`extern` declarations.

| Need | Alternative | Licence |
|---|---|---|
| MQTT | Eclipse Paho C | EPL-2.0 or EDL-1.0 |
| MQTT | libmosquitto | EPL-2.0 or EDL-1.0 |
| CoAP | libcoap | BSD-2-Clause |
| HTTP server | CivetWeb | MIT |
| HTTP server | QTcpServer / QSslServer from Qt Network | LGPL |
| OAuth 2 | QNetworkAccessManager, plus a QTcpServer on localhost for the redirect and QDesktopServices::openUrl | LGPL |
| Charts | qtc's drawing API, or QML Canvas and Qt Quick Shapes | LGPL |

## Check the Qt build you ship

The Qt build matters as well as the modules: a closed-source product must not
ship GPL libraries inside its Qt.

The Qt this repository was developed and tested with comes from conda-forge.
Its FFmpeg reports "GPL version 3 or later" (`avutil_license()`), and Qt
Multimedia's FFmpeg backend loads it. To distribute closed-source software, ship
a Qt whose FFmpeg is built without its GPL parts (FFmpeg's default `configure`,
without `--enable-gpl`), or leave Qt Multimedia's FFmpeg backend out. Check the
licence of every library you ship alongside Qt.
