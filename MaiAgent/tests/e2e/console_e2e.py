# 文件作用：从终端入口执行 MaiAgent 端到端场景并核对真实输出。
# 阅读边界：此文件负责构建或测试流程，不在应用运行时解释为业务逻辑。
# 端到端：把整个产物当交互程序用一遍。
#
# 这是**唯一**的端到端脚本。以前还有四个（m1/m2/m4/m5），验的是"经过 REST + SSE
# 之后行为还对不对"——它们测的是 Electron 那套协议，协议和界面一起撤掉了，
# 脚本跟着撤。剩下的这一份直接对着产物：maiagent-console 只链 maiagent，
# 进程内订阅事件总线，全程没有 socket 服务端、没有 REST、没有 SSE。
#
# 两件事分开验：
#
#   1. 静态：产物的 exe 里翻不到 httplib。
#   2. 动态：聊天、工具、授权、切会话、落库，全部通过 stdin / stdout。
#      能用才算数，编译过不算。
#
# 唯一还剩的 HTTP 是**客户端**那一侧：核心用 libcurl 调模型。
# 这个脚本起的假模型只提供 Responses 端点，与唯一产品协议保持一致。
#
# 一切收发都在 Python 里显式 UTF-8，不经过 shell 传参——
# 这台机器的 bash/PowerShell 会把中文按 GBK 毁掉，之前两次误判服务端有 bug。
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

out = sys.stdout.buffer
fails = []

CONSOLE = os.environ.get(
    "MAI_CONSOLE_BIN",
    os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "build", "bin",
                                 "maiagent-console.exe" if os.name == "nt" else "maiagent-console")))


def say(m):
    out.write((m + "\n").encode("utf-8"))
    out.flush()


def check(name, cond, extra=""):
    say("  [%s] %s%s" % ("OK" if cond else "FAIL", name, ("  " + extra) if extra else ""))
    if not cond:
        fails.append(name)
        if "screen" in globals():
            say("    console tail: " + repr(text()[-2000:]))


# ── 1. 静态：产物里没有 HTTP 服务端 ──────────────────────────────
say("== 1. the shipped binary carries no HTTP server ==")

with open(CONSOLE, "rb") as f:
    binary = f.read()
# 对照组是 MaiModelClientTests：它**故意**链了 httplib，拿来当假的模型服务端。
# 只说"console 里没有"是不够的——万一这个标记本来就编不进任何 exe，
# 那这条检查恒为真，等于什么都没验（规范第 13 节）。
# 顺带也把话说清楚了：httplib 在这个仓库里只剩测试夹具这一个身份。
fixture = CONSOLE.replace("maiagent-console", "MaiModelClientTests")
with open(fixture, "rb") as f:
    fixture_binary = f.read()

check("the product has no httplib", binary.count(b"httplib") == 0)
check("but the test fixture does (so the probe works)", fixture_binary.count(b"httplib") > 0,
      "%d hits" % fixture_binary.count(b"httplib"))
# 适配器那几条路由。别写成 b"/api/" —— 帮助文本里的 GLM 地址
# （.../api/paas/v4）也会撞上，那是**客户端**要调的地址，不是路由。
for route in (b"/api/session", b"/api/event", b"/api/permission"):
    check("no " + route.decode() + " route", binary.count(route) == 0)

# 反过来，客户端那一侧必须还在：核心得能调模型。
check("client-side SSE is still there", binary.count(b"text/event-stream") > 0)
say("")


# ── 假模型：按剧本依次返回"要调 write"和"最终回答" ────────────
# 每个元素: (text, tool_name, tool_args) 或 (text, tool_name, tool_args, reasoning)
SCRIPTS = []
served = {"n": 0}
served_lock = threading.Lock()

# 每个字之间停多久。默认 0（跑得越快越好），验 busy 和 interrupt 时调大——
# 那两件事都要求"一轮正在跑"这个状态能持续到下一条命令送进去。
char_delay = {"s": 0.0}


class ModelHandler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        if self.path != "/responses":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n)
        with served_lock:
            idx = served["n"]
            served["n"] += 1
            ModelHandler.bodies.append(body)
        entry = SCRIPTS[idx] if idx < len(SCRIPTS) else ("", "", "")
        text, tool, args = entry[0], entry[1], entry[2]
        # 第四项是独立 reasoning item，不能拼入可见助手正文。
        reasoning = entry[3] if len(entry) > 3 else ""

        def frame(event):
            return ("data: " + json.dumps(event, ensure_ascii=False)
                    + "\n\n").encode("utf-8")

        items = []
        reasoning_frames = []
        if reasoning:
            items.append({"type": "reasoning", "id": "rs_%d" % idx, "summary": [],
                          "content": [{"type": "reasoning_text", "text": reasoning}]})
            reasoning_frames = [frame({"type": "response.reasoning_text.delta",
                                       "output_index": 0, "content_index": 0, "delta": piece})
                                for piece in reasoning]
        tool_frame = b""
        if tool:
            item = {"type": "function_call", "id": "fc_%d" % idx,
                    "call_id": "call_%d" % idx, "name": tool, "arguments": args}
            tool_frame = frame({"type": "response.output_item.done",
                                "output_index": len(items), "item": item})
            items.append(item)
        text_index = len(items)
        if text:
            items.append({"type": "message", "id": "msg_%d" % idx, "role": "assistant",
                          "status": "completed",
                          "content": [{"type": "output_text", "text": text}]})
        text_frames = [frame({"type": "response.output_text.delta", "output_index": text_index,
                              "content_index": 0, "delta": piece}) for piece in text]
        # 第五项仅供断流用例：以合法 HTTP EOF 结束，却不给模型协议终止帧。
        done = b"" if len(entry) > 4 and entry[4] else frame({
            "type": "response.completed",
            "response": {"id": "resp_%d" % idx, "status": "completed", "output": items}})
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        # HTTP/1.0 默认靠关闭连接界定正文；本地 Windows 定点复现会在首字后报告接收错误。
        # 声明精确长度，使正常完成由已收到的字节决定，仍逐字流式发送。
        self.send_header("Content-Length", str(sum(map(len, reasoning_frames)) + len(tool_frame)
                                              + sum(map(len, text_frames)) + len(done)))
        self.end_headers()
        if reasoning:
            for encoded in reasoning_frames:
                self.wfile.write(encoded)
            self.wfile.flush()
        if tool:
            self.wfile.write(tool_frame)
        if text:
            # 一个字一个字吐，顺便验多字节字符被切在 chunk 边界上还能拼回来。
            for encoded in text_frames:
                try:
                    self.wfile.write(encoded)
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
                    # 被 /interrupt 掐断了。核心把连接关掉正是它该做的事，
                    # 这里不是失败——假模型安静退出就行，别把栈打到屏幕上。
                    return
                time.sleep(char_delay["s"])
        self.wfile.write(done)
        self.wfile.flush()


ModelHandler.bodies = []
model = ThreadingHTTPServer(("127.0.0.1", 0), ModelHandler)
model_port = model.server_address[1]
threading.Thread(target=model.serve_forever, daemon=True).start()

workspace = tempfile.mkdtemp(prefix="maiagent-m6-")
database = os.path.join(workspace, "agent.db")

# 载荷，不是文案：合起来是 "你好！我是跑在 C++ 核心里的测试模型 🙂"
GREETING = ("\u4f60\u597d\uff01\u6211\u662f\u8dd1\u5728 C++ "
            "\u6838\u5fc3\u91cc\u7684\u6d4b\u8bd5\u6a21\u578b \U0001F642")

SCRIPTS[:] = [
    (GREETING, "", ""),                                                    # 第一轮：纯聊天
    ("", "tool_search", json.dumps({"select": ["file_write"]})),           # 先发现，再在下一请求调用
    ("", "file_write", json.dumps({"path": "note.txt", "content": "written after approval"})),
    ("Done, I wrote note.txt.", "", ""),                                   # 第二轮的收尾
]

say("== 2. drive the product as an interactive program ==")
say("  workspace " + workspace)
say("  fake model on 127.0.0.1:%d" % model_port)

proc = subprocess.Popen(
    [CONSOLE,
     "--dir", workspace,
     "--db", database,
     "--model-url", "http://127.0.0.1:%d" % model_port,
     "--model-key", "fake", "--model", "glm-5.3"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

# stdout 是管道，所以控制台那边走的是"原样写 UTF-8 字节"那条路
# （WriteConsoleW 对管道无效，见 MaiConsoleMain.cpp 里 writeUtf8 的注释）。
screen = []
screen_lock = threading.Lock()


def pump():
    while True:
        chunk = proc.stdout.read(1)
        if not chunk:
            return
        with screen_lock:
            screen.append(chunk)


threading.Thread(target=pump, daemon=True).start()


def text():
    with screen_lock:
        return b"".join(screen).decode("utf-8", "replace")


def wait_for(needle, timeout=20):
    """等屏幕上出现某段文字。只能用在"这段文字以前没出现过"的地方。"""
    return wait_after(needle, 0, timeout)


def wait_after(needle, mark, timeout=20):
    """等屏幕上 mark 之后出现某段文字。

    验一条命令的输出必须用这个，不能用 wait_for：屏幕上早就有过同样的字了
    （工具卡在流式输出里就打过一遍），那样的检查会立刻为真——其实命令的输出
    根本还没打出来，等于什么都没等到。这就是规范第 13 节说的
    "这套观测能不能看见目标"，这里差点踩进去。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in text()[mark:]:
            return True
        time.sleep(0.05)
    return False


def prompts_seen():
    """屏幕上出现过几个提示符。

    控制台每收到一条 SessionIdle 就打一个提示符，所以数提示符就是数轮次——
    m2 里那个 wait_turns 数的是 session.idle 事件，这里数的是它在屏幕上的投影。

    **要数个数，不能搜"有没有"。** 提示符不止轮次结束时会打：命令执行完打一个，
    prompt 被 Busy 顶回去也打一个。搜的话会撞上更早的那些，检查立刻为真，
    而轮次还在跑——下一条命令就被 Busy 顶回去，症状是"没反应"。踩过一次。"""
    return text().count("\n> ")


def wait_prompts(target, timeout=20):
    """等提示符个数涨到 target。

    不能拿"答案的最后几个字出现了"当轮次结束：那时 turn 还在收尾
    （落库、起标题、销毁工作线程），会话仍然是"在跑"。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if prompts_seen() >= target:
            return True
        time.sleep(0.05)
    return False


def wait_quiet(seconds=0.5, timeout=25):
    """等输出安静下来，返回最后一次有新字节的时刻。

    判断"一轮是不是被打断了"必须用它，不能用"等提示符"。红测里发现提示符
    不是独占信号：/help 这类命令执行完也会打一个。把 /interrupt 换成 /help
    之后，"等到提示符了"照样为真——轮次其实一直在跑，检查什么都没验到。
    输出停没停是骗不了的。"""
    deadline = time.time() + timeout
    last_len = len(text())
    last_change = time.time()
    while time.time() < deadline:
        now_len = len(text())
        if now_len != last_len:
            last_len = now_len
            last_change = time.time()
        elif time.time() - last_change >= seconds:
            return last_change
        time.sleep(0.05)
    return time.time()


def send(line):
    proc.stdin.write((line + "\n").encode("utf-8"))
    proc.stdin.flush()


# ── 开场 ────────────────────────────────────────────────────
check("banner says it links the library only", wait_for("links maiagent only"))
check("banner shows the session id", "ses_" in text())
check("banner shows the database", database in text())
check("the initial prompt is ready", wait_prompts(1))

# ── 第一轮：纯聊天，验流式 ──────────────────────────────────
turns = prompts_seen()
send("\u4f60\u597d")
check("streams the full multi-byte answer", wait_for(GREETING), repr(text()[-120:]))
# 最后正文仍可能早于 [DONE]、落库和 SessionIdle。先等本轮提示符，
# 再发送下一请求；否则 Busy 的提示符还会污染后面各轮的计数基线。
check("the first chat went idle", wait_prompts(turns + 1))

# ── 第二轮：工具 + 授权 ────────────────────────────────────
mark = len(text())
turns = prompts_seen()
send("write note.txt")
check("tool card shows up as pending", wait_for("[tool] file_write  pending"))
check("tool card carries the arguments", "note.txt" in text()[mark:])
check("console says how to answer", wait_for("/y approve"))
check("the file is not there yet", not os.path.exists(os.path.join(workspace, "note.txt")))

mark = len(text())
send("/y")
check("approval is echoed", wait_after("approved", mark))
check("tool runs to completion", wait_after("[tool] file_write  completed", mark))
check("model gets the last word", wait_after("Done, I wrote note.txt.", mark))
check("the note turn went idle", wait_prompts(turns + 1))

note = os.path.join(workspace, "note.txt")
check("the file really got written", os.path.exists(note))
if os.path.exists(note):
    with open(note, "rb") as f:
        check("with the right content", f.read() == b"written after approval")

# 回灌给模型的那一轮里必须带上 tool 结果，否则它是在凭空接话。
with served_lock:
    bodies = list(ModelHandler.bodies)
check("four model calls include one discovery request", len(bodies) == 4, "%d" % len(bodies))
if len(bodies) == 4:
    first = json.loads(bodies[0].decode("utf-8"))
    declared = json.loads(bodies[2].decode("utf-8"))
    fed = json.loads(bodies[3].decode("utf-8"))
    check("initial tool set remains small", len(first.get("tools", [])) <= 8)
    check("write is not advertised before discovery", "file_write" not in
          [tool.get("name") for tool in first.get("tools", [])])
    check("discovered schema appends without changing earlier declarations",
          declared.get("tools", [])[:len(first.get("tools", []))] == first.get("tools", []) and
          "file_write" in [tool.get("name") for tool in declared.get("tools", [])])
    check("selected declarations remain fixed for the next tool follow-up",
          declared.get("tools", []) == fed.get("tools", []))
    types = [item.get("type") for item in fed.get("input", [])]
    check("the tool result was fed back", "function_call_output" in types, repr(types))

# ── 基础补丁工具直接可用，不依赖 shell 中存在同名程序 ─────────
with served_lock:
    served["n"] = 0
    patch_body_mark = len(ModelHandler.bodies)
PATCH = ("*** Begin Patch\n*** Update File: note.txt\n@@\n"
         "-written after approval\n+updated with apply_patch\n*** End Patch\n")
SCRIPTS[:] = [("", "apply_patch", json.dumps({"patch": PATCH})),
              ("Applied the Codex patch.", "", "")]
mark = len(text())
turns = prompts_seen()
send("update note.txt with apply_patch")
check("canonical patch tool is pending without metadata discovery",
      wait_after("[tool] apply_patch  pending", mark))
send("/y")
check("canonical patch tool completes", wait_after("[tool] apply_patch  completed", mark))
check("patch turn gets its final answer", wait_after("Applied the Codex patch.", mark))
check("patch turn went idle", wait_prompts(turns + 1))
with open(note, "rb") as updated:
    check("patch edits existing bytes and adds the Codex final newline",
          updated.read() == b"updated with apply_patch\n")
with served_lock:
    patch_requests = list(ModelHandler.bodies[patch_body_mark:])
check("patch uses only the apply and follow-up model requests", len(patch_requests) == 2)
if patch_requests:
    declarations = json.loads(patch_requests[0].decode("utf-8")).get("tools", [])
    check("canonical patch is in the initial selected declarations",
          "apply_patch" in [tool.get("name") for tool in declarations])

# ── 上下文是累积的 ────────────────────────────────────────
# 模型拿到的不能只是最新那一句。这条以前在 m2 里，靠翻 REST 请求体验；
# 现在直接翻假模型收到的请求体，和界面无关。
with served_lock:
    # SCRIPTS 重新编号了，索引必须跟着归零——不归零的话 idx 直接越界，
    # 假模型走"剧本用完"那条分支返回空答案，而症状是"模型不说话"，很难一眼看出。
    served["n"] = 0
SCRIPTS[:] = [("I still remember.", "", "")]

mark = len(text())
turns = prompts_seen()
send("what did you say first")
check("third turn answered", wait_after("I still remember.", mark))

with served_lock:
    latest = json.loads(ModelHandler.bodies[-1].decode("utf-8"))
contents = []
for item in latest.get("input", []):
    content = item.get("content", "")
    if isinstance(content, str):
        contents.append(content)
    elif isinstance(content, list):
        contents.extend(part["text"] for part in content if isinstance(part.get("text"), str))
check("history carries the first answer", GREETING in contents,
      "%d messages" % len(contents))
check("history carries the latest prompt", "what did you say first" in contents)
check("third turn went idle", wait_prompts(turns + 1))

# ── /a 之后不再问 ─────────────────────────────────────────
# "本会话都允许"必须真的止住后续的询问，否则这个选项只是换了种说法的"允许一次"。
with served_lock:
    # SCRIPTS 重新编号了，索引必须跟着归零——不归零的话 idx 直接越界，
    # 假模型走"剧本用完"那条分支返回空答案，而症状是"模型不说话"，很难一眼看出。
    served["n"] = 0
SCRIPTS[:] = [
    ("", "file_write", json.dumps({"path": "always-1.txt", "content": "first"})),
    ("First one done.", "", ""),
    ("", "file_write", json.dumps({"path": "always-1.txt", "content": "second"})),
    ("Second one done.", "", ""),
    ("", "file_write", json.dumps({"path": "always-2.txt", "content": "third"})),
    ("Third one done.", "", ""),
]

mark = len(text())
turns = prompts_seen()
send("write always-1.txt")
check("it asks the first time", wait_after("needs approval", mark))
mark = len(text())
send("/a")
check("first write went through", wait_after("First one done.", mark))
check("that turn went idle", wait_prompts(turns + 1))

mark = len(text())
turns = prompts_seen()
send("update always-1.txt")
check("second write finished", wait_after("Second one done.", mark))
check("and the same file did NOT ask again", "needs approval" not in text()[mark:])
with open(os.path.join(workspace, "always-1.txt"), encoding="utf-8") as updated:
    check("the approved file was updated", updated.read() == "second")
check("the always-turn went idle", wait_prompts(turns + 1))

mark = len(text())
turns = prompts_seen()
send("write always-2.txt")
check("a different file asks for approval", wait_after("needs approval", mark))
mark = len(text())
send("/a")
check("the new file was written after approval", wait_after("Third one done.", mark))
check("the new file exists", os.path.exists(os.path.join(workspace, "always-2.txt")))
check("the new-file turn went idle", wait_prompts(turns + 1))

# ── 一轮在跑的时候不收第二条 ──────────────────────────────
# 不排队是故意的：排队的消息在界面上和"发出去了"长得一模一样，用户会以为丢了。
with served_lock:
    # SCRIPTS 重新编号了，索引必须跟着归零——不归零的话 idx 直接越界，
    # 假模型走"剧本用完"那条分支返回空答案，而症状是"模型不说话"，很难一眼看出。
    served["n"] = 0
SCRIPTS[:] = [("this answer comes out slowly, one character at a time", "", "")]
char_delay["s"] = 0.08

mark = len(text())
turns = prompts_seen()
send("answer slowly")
check("the slow turn started", wait_after("this answer", mark))
send("cut in line")
check("the second prompt is rejected, not queued", wait_after("(try /interrupt)", mark))
check("turn still finishes", wait_after("one character at a time", mark))
# +2 而不是 +1：被顶回去的那条 prompt 自己也打了一个提示符。
check("the slow turn went idle", wait_prompts(turns + 2))

# ── /interrupt ────────────────────────────────────────────
with served_lock:
    # SCRIPTS 重新编号了，索引必须跟着归零——不归零的话 idx 直接越界，
    # 假模型走"剧本用完"那条分支返回空答案，而症状是"模型不说话"，很难一眼看出。
    served["n"] = 0
SCRIPTS[:] = [("counting one two three four five six seven eight nine ten", "", "")]

# 整段答案按 char_delay 算要 4 秒多，打断必须明显快于它。
full_answer_seconds = len(SCRIPTS[0][0]) * char_delay["s"]

mark = len(text())
started = time.time()
send("count for me")
check("the turn to interrupt started", wait_after("counting", mark))
send("/interrupt")

# 量的是"输出什么时候停的"，不是"什么时候出现了提示符"——理由见 wait_quiet。
stopped_at = wait_quiet()
check("output stopped well before the answer would have finished",
      stopped_at - started < full_answer_seconds * 0.7,
      "%.1fs of %.1fs" % (stopped_at - started, full_answer_seconds))
check("the rest never came out", "nine ten" not in text()[mark:],
      repr(text()[mark:][-80:]))
char_delay["s"] = 0.0

# ── 思考过程不能当答案打出来 ──────────────────────────────
# 这一段是拿真实 GLM-5.3 跑出来的 bug 变成的回归用例。
#
# 当时的症状：模型的英文思考过程被原样打成了正文，还和答案直接粘在一起。
# 根因在核心不在控制台——MaiTurnRunner 广播 message.part.updated 时，
# 那个 part 还没入库（规范第 10 节"先落库再广播"只在工具那条路径上落实了）。
# 订阅方拿 id 去查查不到，就分不出这是正文还是思考——两者的增量 field 都是 "text"。
#
# 假模型以前不发 reasoning_text，所以这个洞测不到。现在发了。
DRAFT = "I should answer briefly. No tools needed here."
ANSWER = "Briefly: yes."
with served_lock:
    served["n"] = 0
SCRIPTS[:] = [(ANSWER, "", "", DRAFT)]

mark = len(text())
turns = prompts_seen()
send("think before you answer")
check("the answer came out", wait_after(ANSWER, mark))
check("that turn went idle", wait_prompts(turns + 1))
check("the draft was NOT printed as the answer", DRAFT not in text()[mark:],
      repr(text()[mark:][:160]))
check("but it did say the model is thinking", "[thinking...]" in text()[mark:])

# 反过来：开了 --reasoning 就该看得见。不验这一条的话，
# "没打出来"可能只是因为核心根本没把 reasoning 传上来——那是另一种坏法。
with served_lock:
    served["n"] = 0
SCRIPTS[:] = [(ANSWER, "", "", DRAFT)]
with_reasoning = subprocess.run(
    [CONSOLE, "--dir", workspace, "--model-url", "http://127.0.0.1:%d" % model_port,
     "--model-key", "fake", "--reasoning"],
    input="think before you answer\n/quit\n".encode("utf-8"),
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
shown = with_reasoning.stdout.decode("utf-8", "replace")
check("--reasoning shows the draft", DRAFT in shown, repr(shown[-200:]))
check("--reasoning still shows the answer", ANSWER in shown)

# ── 同一进程/同一 turn 的断流恢复 ────────────────────────────
# 直接驱动当前构建的 CLI；不允许把失败草稿作为助手历史再发给假模型。
FAILED_DRAFT = "UNCONFIRMED_LOCAL_DRAFT_E2E"
RECOVERED = "Completed after the interrupted model response."
with served_lock:
    served["n"] = 0
    body_mark = len(ModelHandler.bodies)
SCRIPTS[:] = [(FAILED_DRAFT, "", "", "", True), (RECOVERED, "", "")]
mark = len(text())
turns = prompts_seen()
send("Recover an interrupted response")
check("partial draft remains visible", wait_after(FAILED_DRAFT, mark))
check("host reports model recovery", wait_after("[model] Reconnecting to model 1/2", mark))
check("the same turn completes after reconnecting", wait_after(RECOVERED, mark))
check("recovered turn returns to idle", wait_prompts(turns + 1))
check("recovery is not exposed as a terminal error", "[error]" not in text()[mark:])
with served_lock:
    recovery_requests = ModelHandler.bodies[body_mark:]
check("exactly two HTTP attempts", len(recovery_requests) == 2)
check("retry excludes the unconfirmed assistant draft",
      len(recovery_requests) == 2 and FAILED_DRAFT.encode("utf-8") not in recovery_requests[1])

# ── 查询类命令 ────────────────────────────────────────────
mark = len(text())
send("/sessions")
check("/sessions lists the current one", wait_after("ses_", mark))

mark = len(text())
send("/history")
check("/history shows the user's words", wait_after("user", mark))
check("/history shows the tool call", wait_after("[tool] file_write", mark))

mark = len(text())
send("/help")
check("/help lists the commands", wait_after("/interrupt", mark))

mark = len(text())
send("/nonsense")
check("unknown commands are rejected, not sent to the model",
      wait_after("unknown command /nonsense", mark))

# ── 新建会话 ──────────────────────────────────────────────
mark = len(text())
send("/new")
check("/new reports a second session", wait_after("new session ses_", mark))

# ── 退出 ──────────────────────────────────────────────────
send("/quit")
try:
    code = proc.wait(timeout=20)
except subprocess.TimeoutExpired:
    proc.kill()
    code = -1
check("/quit exits cleanly", code == 0, "exit=%d" % code)
say("")


# ── 3. 落库了吗 ────────────────────────────────────────────
# 控制台进程已经退了，数据库里应该留着两个会话和第一个会话的全部消息。
# 用另一个进程去读——这里就借 maiagent-console 自己：同一个库，同一个文件。
say("== 3. what the console wrote is still there ==")
check("database file exists", os.path.exists(database))

reread = subprocess.run(
    [CONSOLE, "--dir", workspace, "--db", database],
    input="/sessions\n/quit\n".encode("utf-8"),
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
reopened = reread.stdout.decode("utf-8", "replace")
# 重开一次会再建一个会话（控制台一启动就建），所以至少三个。
check("the old sessions came back", reopened.count("ses_") >= 3,
      "%d mentions" % reopened.count("ses_"))

say("")


# ── 4. 硬杀之后数据还在吗 ──────────────────────────────────
# 上面那一段是**正常退出**：析构跑过、SQLite 干净关闭。真实世界不长这样——
# 用户直接叉掉窗口、进程被杀、机器断电。
#
# 这一段用 terminate() 硬杀，一个析构函数都不跑，然后重开看数据还在不在。
# 验证完成 item 的 JSONL 权威提交和 SQLite 投影恢复。可见正文的尾字不等于
# item 已完成：必须先观察本轮 SessionIdle 提示符，再硬杀，不能误测 RAM 草稿持久化。
say("== 4. survives a hard kill ==")

workspace2 = tempfile.mkdtemp(prefix="maiagent-kill-")
database2 = os.path.join(workspace2, "agent.db")
served["n"] = 0
SURVIVOR = "this line must survive a hard kill"
SCRIPTS[:] = [(SURVIVOR, "", "")]

victim = subprocess.Popen(
    [CONSOLE, "--dir", workspace2, "--db", database2,
     "--model-url", "http://127.0.0.1:%d" % model_port, "--model-key", "fake"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

seen = []


def pump_victim():
    while True:
        chunk = victim.stdout.read(1)
        if not chunk:
            return
        seen.append(chunk)


threading.Thread(target=pump_victim, daemon=True).start()
victim.stdin.write(b"remember this\n")
victim.stdin.flush()

deadline = time.time() + 20
while time.time() < deadline:
    completed_screen = b"".join(seen).decode("utf-8", "replace")
    if SURVIVOR in completed_screen and completed_screen.count("\n> ") >= 2:
        break
    time.sleep(0.05)
victim_screen = b"".join(seen).decode("utf-8", "replace")
check("the answer came through before the kill", SURVIVOR in victim_screen)
check("completed item was acknowledged before the kill", victim_screen.count("\n> ") >= 2)

old_session = ""
for token in victim_screen.split():
    if token.startswith("ses_"):
        old_session = token
        break
check("captured the session id", old_session.startswith("ses_"), old_session)

# 硬杀：没有 /quit，没有析构，没有干净关库。
victim.terminate()
victim.wait(timeout=10)

revived = subprocess.run(
    [CONSOLE, "--dir", workspace2, "--db", database2],
    input=("/use %s\n/history\n/quit\n" % old_session).encode("utf-8"),
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
revived_screen = revived.stdout.decode("utf-8", "replace")
check("the killed session came back", ("now talking to " + old_session) in revived_screen)
check("and its answer is still in the history", SURVIVOR in revived_screen,
      repr(revived_screen[-200:]))

shutil.rmtree(workspace, ignore_errors=True)
shutil.rmtree(workspace2, ignore_errors=True)

say("")
say("-" * 40)
if fails:
    say("console end-to-end: %d checks FAILED" % len(fails))
    for f in fails:
        say("  - " + f)
    sys.exit(1)
say("console end-to-end: all checks passed")
