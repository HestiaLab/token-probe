#!/usr/bin/env python3
"""CLI 机器可读输出（XML 和 JSON）的契约测试。

输出文档是桌面和 Web 宿主消费的接口，因此检查的是序列化器绝不能丢失的性质
——文档可以解析、值完整到达——而不是逐字节渲染结果。
`xml.etree` 和 `json` 是裁判：供应商文本只要能让其中任一解析失败，文档就是坏的。

成功路径通过回环地址上预置的 DeepSeek 响应端到端驱动，因为转义规则只有用
供应商提供的文本才能真正检验。夹具币种包含一对 XML 元字符、一个双引号和一个
原始 C0 控制字节（作为 JSON ``\\u`` 转义合法，但 XML 1.0 完全无法表示）。

比较前两种格式都会归约为同一结构，因此某一格式单独偏移时会在本测试失败，
而不是在消费方暴露。

直接运行：
    python3 tests/test_cli_output.py [path/to/tokenprobe[.exe]]
"""

import http.server
import json
import os
import subprocess
import sys
import threading
import xml.etree.ElementTree as ET
from pathlib import Path

# 币种字符串是刻意构造的敌意输入。`\u0001` 以 JSON 转义写出
# （因此字面量中是双反斜杠），使解析器把原始控制字节交给序列化器。
_CANNED_BODY = (
    b'{"is_available":true,"balance_infos":['
    b'{"currency":"C<&\\"\\u0001NY","total_balance":"110.00",'
    b'"granted_balance":"10.00","topped_up_balance":"100.00"},'
    b'{"currency":"USD","total_balance":"0.00"}]}'
)

# 各格式对该币种字符串的预期结果。XML 1.0 无法表示控制字节，
# 因此替换掉；JSON 可以原样承载。
_HOSTILE_XML = 'C<&"\ufffdNY'
_HOSTILE_JSON = 'C<&"\x01NY'


class _Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802 - 名称由 http.server 固定
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(_CANNED_BODY)))
        self.end_headers()
        self.wfile.write(_CANNED_BODY)

    def log_message(self, *args):
        pass


def _pin_utf8_console() -> None:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")


class Case:
    def __init__(self, name: str):
        self.name = name
        self.failures: list[str] = []

    def check(self, what: str, ok: bool, detail: str = "") -> None:
        if not ok:
            self.failures.append(what + (f" ({detail})" if detail else ""))

    def eq(self, what: str, got, want) -> None:
        self.check(what, got == want, f"got {got!r}, want {want!r}")

    def done(self) -> bool:
        if self.failures:
            print(f"FAIL {self.name}")
            for failure in self.failures:
                print(f"       - {failure}")
            return False
        print(f"ok   {self.name}")
        return True


def _run(exe: Path, args: list[str]) -> subprocess.CompletedProcess:
    env = {k: v for k, v in os.environ.items() if k != "DEEPSEEK_API_KEY"}
    return subprocess.run([str(exe)] + args, capture_output=True, env=env)


def _forbidden_control(blob: bytes) -> bool:
    """XML 1.0 允许 TAB、LF 和 CR；其他 C0 码点均非法。"""
    return any(c < 0x20 and c not in (9, 10, 13) for c in blob)


def _decodes(blob: bytes) -> bool:
    try:
        blob.decode("utf-8")
        return True
    except UnicodeDecodeError:
        return False


def _as_bool_text(value) -> "str | None":
    """把 XML 属性、JSON 布尔值和 JSON null 归一为同一套取值。"""
    if value is None:
        return None
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def _as_text(value) -> "str | None":
    """把 XML 属性或 JSON 数字统一为字符串。"""
    return None if value is None else str(value)


def model_xml(blob: bytes):
    """把 XML 文档归约为普通 dict；文档损坏时抛出异常。"""
    root = ET.fromstring(blob.decode("utf-8"))
    result = root.find("result")
    capabilities = result.find("capabilities") if result is not None else None
    error = result.find("error") if result is not None else None
    metrics_parent = result.find("metrics") if result is not None else None
    metric_nodes = [] if metrics_parent is None else metrics_parent.findall("metric")

    return {
        "root_tag": root.tag,
        "schema": root.get("schema"),
        "version": root.get("version"),
        "provider": None if result is None else result.get("provider"),
        "display_name": None if result is None else result.get("display-name"),
        "ok": None if result is None else result.get("ok"),
        "http_status": None if result is None else result.get("http-status"),
        "latency_ms": None if result is None else result.get("latency-ms"),
        "observed_at": None if result is None else _as_text(result.get("observed-at")),
        "available": None if result is None else result.get("available"),
        "capabilities": None if capabilities is None else {
            "credit": capabilities.get("credit"),
            "spend": capabilities.get("spend"),
            "quota": capabilities.get("quota"),
        },
        "metrics": [
            {
                "kind": node.get("kind"),
                "id": node.get("id"),
                "label": node.get("label"),
                "period_start": node.get("period-start"),
                "period_end": node.get("period-end"),
                "unit": node.find("amount").get("unit"),
                "currency": node.find("amount").get("currency"),
                "amount": node.findtext("amount"),
            }
            for node in metric_nodes
        ],
        "note": None if result is None else result.findtext("note"),
        "raw_response": None if result is None else result.findtext("raw-response"),
        "error": None if error is None else {
            "code": error.get("code"),
            "http_status": error.get("http-status"),
            "message": error.findtext("message"),
            "hint": error.findtext("hint"),
        },
    }


def model_json(blob: bytes):
    """把 JSON 文档归约为与 model_xml() 相同的 dict 结构。"""
    doc = json.loads(blob.decode("utf-8"))
    result = doc["result"]
    capabilities = result.get("capabilities")

    return {
        "root_tag": "tokenprobe",
        "schema": _as_bool_text(doc.get("schema")),
        "version": doc.get("version"),
        "provider": result.get("provider"),
        "display_name": result.get("display_name"),
        "ok": _as_bool_text(result.get("ok")),
        "http_status": _as_bool_text(result.get("http_status")),
        "latency_ms": _as_bool_text(result.get("latency_ms")),
        "observed_at": _as_text(result.get("observed_at")),
        "available": _as_bool_text(result.get("available")),
        "capabilities": None if capabilities is None else {
            "credit": _as_bool_text(capabilities.get("credit")),
            "spend": _as_bool_text(capabilities.get("spend")),
            "quota": _as_bool_text(capabilities.get("quota")),
        },
        "metrics": [
            {
                "kind": metric.get("kind"),
                "id": metric.get("id"),
                "label": metric.get("label"),
                "period_start": metric.get("period_start"),
                "period_end": metric.get("period_end"),
                "unit": metric.get("unit"),
                "currency": metric.get("currency"),
                "amount": metric.get("amount"),
            }
            for metric in result.get("metrics", [])
        ],
        "note": result.get("note"),
        "raw_response": result.get("raw_response"),
        "error": None if "error" not in result else {
            "code": result["error"].get("code"),
            "http_status": _as_bool_text(result["error"].get("http_status")),
            "message": result["error"].get("message"),
            "hint": result["error"].get("hint"),
        },
    }


_MODEL = {"xml": model_xml, "json": model_json}


def _flag_for(fmt: str) -> str:
    return f"--{fmt}"


def case_error_path(exe: Path, fmt: str) -> bool:
    c = Case(f"error path, {fmt}")
    proc = _run(exe, ["--provider", "deepseek", _flag_for(fmt)])
    c.eq("exit code", proc.returncode, 1)
    c.check("stdout is UTF-8", _decodes(proc.stdout))

    model = _MODEL[fmt](proc.stdout)
    c.eq("root element", model["root_tag"], "tokenprobe")
    c.eq("schema", model["schema"], "1")
    c.check("version is present", bool(model["version"]))
    c.eq("provider", model["provider"], "deepseek")
    c.eq("ok", model["ok"], "false")
    c.check("error detail is present", model["error"] is not None)
    if model["error"] is not None:
        c.eq("error code", model["error"]["code"], "INVALID_KEY")
        c.check("error message is non-empty", bool(model["error"]["message"]))
    return c.done()


def case_success_path(exe: Path, base_url: str, fmt: str) -> bool:
    c = Case(f"success path, {fmt}")
    proc = _run(exe, ["--provider", "deepseek", "--api-key", "sk-test",
                      "--base-url", base_url, _flag_for(fmt)])
    c.eq("exit code", proc.returncode, 0)
    c.check("stdout is UTF-8", _decodes(proc.stdout))

    model = _MODEL[fmt](proc.stdout)
    c.eq("schema", model["schema"], "1")
    c.eq("provider", model["provider"], "deepseek")
    c.eq("display_name", model["display_name"], "DeepSeek")
    c.eq("ok", model["ok"], "true")
    c.eq("http_status", model["http_status"], "200")
    c.check("latency_ms is numeric", (model["latency_ms"] or "").isdigit(),
            repr(model["latency_ms"]))
    c.check("observed_at is numeric", (model["observed_at"] or "").isdigit(),
            repr(model["observed_at"]))
    c.eq("available", model["available"], "true")

    c.eq("capabilities", model["capabilities"],
         {"credit": "true", "spend": "false", "quota": "false"})

    metrics = model["metrics"]
    c.eq("metric count", len(metrics), 4)
    if metrics:
        first = metrics[0]
        c.eq("first kind", first["kind"], "credit")
        c.eq("first id", first["id"], "total_balance")
        c.eq("first label", first["label"], "Total balance")
        c.eq("amount is the exact decimal", first["amount"], "110.00")
        c.eq("amount unit", first["unit"], "currency")
        c.eq("absent period start", first["period_start"], None)
        c.eq("absent period end", first["period_end"], None)
        c.eq("hostile currency survives",
             first["currency"], _HOSTILE_XML if fmt == "xml" else _HOSTILE_JSON)

    c.check("note is present", bool(model["note"]))

    if fmt == "xml":
        c.check("no forbidden control bytes", not _forbidden_control(proc.stdout))
        c.check("control byte became U+FFFD", b"\xef\xbf\xbd" in proc.stdout)
    else:
        # JSON 可以承载该字节；关键是文档仍能解析，这一点已由 model_json() 证明。
        c.check("control byte is escaped, not dropped", b"\\u0001" in proc.stdout)
    return c.done()


def case_formats_agree(exe: Path, base_url: str) -> bool:
    c = Case("the two formats carry the same fields")
    args = ["--provider", "deepseek", "--api-key", "sk-test", "--base-url", base_url]
    xml_model = model_xml(_run(exe, args + ["--xml"]).stdout)
    json_model = model_json(_run(exe, args + ["--json"]).stdout)

    c.eq("same keys", sorted(xml_model), sorted(json_model))
    c.eq("same schema", xml_model["schema"], json_model["schema"])
    c.eq("same capabilities", xml_model["capabilities"], json_model["capabilities"])
    c.eq("same note", xml_model["note"], json_model["note"])
    c.eq("same metric count", len(xml_model["metrics"]), len(json_model["metrics"]))

    for index, (left, right) in enumerate(zip(xml_model["metrics"], json_model["metrics"])):
        for field in ("kind", "id", "label", "amount", "unit", "period_start", "period_end"):
            c.eq(f"metric {index} field {field}", left[field], right[field])

    # 未上报的可用性在 XML 中省略，在 JSON 中写为 null。
    # 两种格式对“已上报内容”的取值仍必须一致。
    error_models = [
        _MODEL[fmt](_run(exe, ["--provider", "deepseek", _flag_for(fmt)]).stdout)
        for fmt in ("xml", "json")
    ]
    c.eq("error code agrees across formats",
         error_models[0]["error"]["code"], error_models[1]["error"]["code"])
    c.eq("error message agrees across formats",
         error_models[0]["error"]["message"], error_models[1]["error"]["message"])
    return c.done()


def case_raw_and_headers_stay_parseable(exe: Path, base_url: str, fmt: str) -> bool:
    c = Case(f"--raw and --headers keep the {fmt} document parseable")
    proc = _run(exe, ["--provider", "deepseek", "--api-key", "sk-test",
                      "--base-url", base_url, _flag_for(fmt), "--raw", "--headers"])
    c.eq("exit code", proc.returncode, 0)

    model = _MODEL[fmt](proc.stdout)
    c.check("raw response is carried", bool(model["raw_response"]), repr(model["raw_response"]))
    c.check("header diagnostic is not on stdout",
            b"header capture is transport specific" not in proc.stdout)
    c.check("header diagnostic is on stderr",
            b"header capture is transport specific" in proc.stderr)
    return c.done()


def case_format_flags(exe: Path, fmt: str) -> bool:
    c = Case(f"--{fmt} flag behaviour")
    both = [other for other in ("xml", "json") if other != fmt][0]

    proc = _run(exe, ["--provider", "deepseek", _flag_for(fmt), _flag_for(both)])
    c.eq("conflicting formats are rejected", proc.returncode, 2)
    c.check("no document on stdout when the request is invalid",
            not proc.stdout.startswith(b"{"))
    c.check("the conflict is explained", b"mutually exclusive" in proc.stderr)

    proc = _run(exe, ["--provider", "deepseek", _flag_for(fmt), _flag_for(fmt), "--help"])
    c.eq("repeating the same flag is accepted", proc.returncode, 0)

    proc = _run(exe, ["--help"])
    c.check(f"usage advertises --{fmt}", f"--{fmt}".encode() in proc.stdout)
    return c.done()


def case_no_document_in_text_mode(exe: Path, base_url: str) -> bool:
    c = Case("text mode stays human-readable")
    proc = _run(exe, ["--provider", "deepseek", "--api-key", "sk-test", "--base-url", base_url])
    c.eq("exit code", proc.returncode, 0)
    c.check("no XML declaration", b"<?xml" not in proc.stdout)
    c.check("no JSON document", not proc.stdout.lstrip().startswith(b"{"))
    c.check("display name is printed", b"DeepSeek" in proc.stdout)
    return c.done()


def main(argv: list[str]) -> int:
    _pin_utf8_console()
    exe = Path(argv[1]) if len(argv) > 1 else None
    if exe is None:
        print("usage: test_cli_output.py <path-to-tokenprobe>", file=sys.stderr)
        return 2
    if not exe.is_file():
        print(f"tokenprobe executable not found: {exe}", file=sys.stderr)
        return 2

    server = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base_url = f"http://127.0.0.1:{server.server_address[1]}"

    results = []
    try:
        for fmt in ("xml", "json"):
            results.append(case_error_path(exe, fmt))
            results.append(case_success_path(exe, base_url, fmt))
            results.append(case_raw_and_headers_stay_parseable(exe, base_url, fmt))
            results.append(case_format_flags(exe, fmt))
        results.append(case_formats_agree(exe, base_url))
        results.append(case_no_document_in_text_mode(exe, base_url))
    except (ET.ParseError, ValueError) as exc:
        print(f"FAIL a document did not parse: {exc}", file=sys.stderr)
        return 1
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)

    if not all(results):
        print("\nthe CLI's machine-readable output contract is broken", file=sys.stderr)
        return 1
    print("\nall output contract checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
