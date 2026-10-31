# C++20 OpenAI Agent (实验性)

新模块 mymcp::agent 在现有 MCP client/server 上实现 OpenAI Responses 编排，
不替换或复制 MCP 协议、JSON Schema、OAuth 等实现。兼容原项目的离线 CMake 构建。

## 功能

- Responses API（非已废弃的 Assistants API）；函数调用自动执行与多轮历史，离线注入 Model 方便测试。
- MCP Client 复用：Client::process、Client::http、Client::http_oauth。
- 在 Agent 上直接注册 MCP 工具（原有 Server::tool 同一套类型推导和参数校验）。
- Skill：递归发现指定目录中的 SKILL.md，读取 name/description 简单 frontmatter；use_skill 显式启用；**不会执行** Skill 附带的代码或脚本。
- 视觉输入：png/jpeg/webp/gif 文件转 base64 Data URL，走 Responses input_image。
- 音频输入：OpenAI Audio Transcriptions → 普通文本对话；输出：Audio Speech → MP3 文件。**不是**流式语音对话或实时双向语音。
- save_history / load_history 保存多轮上下文；历史仅包含对话和工具结果，不保存 API Key。

## 基本使用

~~~cpp
#include "mymcp/agent.hpp"
using namespace mymcp;

int main() {
    agent::Config config;
    config.model = "gpt-4.1-mini"; // 可改为支持所需能力的其他模型
    // config.api_key = "..."; 默认从 OPENAI_API_KEY 读取（推荐）
    agent::Agent bot(config);

    bot.tool("sum", "加法",
      [](int a, int b) { return a + b; },
      arg<int>("a", "被加数"), arg<int>("b", "加数"));

    auto remote = std::make_shared<Client>(Client::http("http://127.0.0.1:3000/mcp"));
    bot.add_mcp("remote", remote); // 工具名称以 remote__ 开头

    bot.load_skills("./skills");
    bot.use_skill("brief-summary");
    auto answer = bot.ask("计算 25 + 17");
    auto followup = bot.ask("刚才结果是多少？");
    bot.save_history("history.json");

    // bot.ask_image("解释这个截图", "screenshot.png");
    // bot.ask_audio("recording.wav", "请总结这段语音");
    // bot.speak("你好", "reply.mp3");
}
~~~

## 编译和 Demo

~~~sh
cmake -S . -B build
cmake --build build --target run_agent_demo mymcp_agent_tests
ctest --test-dir build -R agent --output-on-failure

# 配置 OPENAI_API_KEY 环境变量后：
./build/run_agent_demo
~~~

run_agent_demo 无命令行参数；可选环境变量：OPENAI_MODEL、
MYAGENT_MCP_URL、MYAGENT_IMAGE、MYAGENT_AUDIO、MYAGENT_SPEECH_OUTPUT。
从仓库根目录运行 Demo，才会自动找到 ./skills。

## 设计与限制

- 命名空间隔离：所有远程工具以 prefix__native_name 暴露给模型；
  不允许同名/超过 64 字符的函数名（特殊字符转下划线）。
- MCP 仍由 mymcp::Client 做握手、列表分页和实际调用。进程内工具也是一个 MCP Server + Client 回环，不存在第二套工具协议实现。
- 多轮保留的是完整 Responses 输入/输出项目，不仅仅是纯文本；设置 store:false，但远程 OpenAI 请求仍需遵循其数据处理政策。
- API 请求和 MCP 工具执行同步进行；Agent 实例不支持并发调用，不提供流式 SSE、浏览器实时语音、自动重试、断点续传或上下文压缩。
- max_tool_rounds 限制自动调用链；异常时对话历史不提交，但**已执行工具的副作用不能撤销**。
- 当前音频输入使用转写后的文字而不是直接理解音色、音乐、语调等音频特征。
- 模型支持范围以 OpenAI API 的实际能力为准，默认模型仅作为示例。
- Skill 是可信提示词输入，加载前应检查内容；它不会获得执行脚本的特权。
- 图片和音频文件上限 20 MiB；MCP 工具返回上限 256 KiB；历史上限可配置。
- API Key 应通过环境变量或安全密钥存储传入，不要写进仓库或输出日志。

后续可扩展：Realtime/WebSocket、流式事件 API、取消令牌、模型厂商接口、
预算管理、上下文剪枝、音频直接输入模型以及 SKILL.md 更完整的 YAML 解析。
