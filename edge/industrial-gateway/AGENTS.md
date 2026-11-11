# Codex 开发规范

## 1. 第三方依赖管理
- 禁止自动安装、下载或引入任何第三方包、库。
- 如果开发过程中需要新增依赖，必须暂停操作，告知我依赖名称和用途，经过我明确同意后才能安装。
- 禁止通过其他方式绕过此限制，例如 CMake FetchContent。

## 2. 开发验证要求
- 开发完成后，只需确保代码没有语法错误和编译错误。
- 不需要编写或执行单元测试、集成测试等测试工作。
- 测试属于独立任务，只有我明确提出要求时才执行。
- 如果编译需要安装额外依赖，暂停并告知我，不得自行安装。

## 3. 开发要求
- 代码要增加必要的中文注释，特别是逻辑复杂的地方

## 4. 不处理内容

include/industrial_gateway/application/ports/IDataQueue.hpp
src/infrastructure/concurrency/BoundedDataQueue.hpp/.cpp
src/application/connector/ConnectionWorker.hpp/.cpp

目前队列满时 push() 会阻塞，可能让单连接停止一直等待
