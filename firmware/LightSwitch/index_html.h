#pragma once

const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>형광등</title>
<style>
  :root { color-scheme: light; }
  * { box-sizing: border-box; }
  body {
    margin: 0; font-family: sans-serif; background: #f3f0e8; color: #1c1915;
  }
  main { max-width: 28rem; margin: 0 auto; padding: 1.25rem; }
  h1 { font-size: 1.35rem; margin: 0 0 0.25rem; }
  p { margin: 0.35rem 0; line-height: 1.45; }
  .muted { color: #5e584e; font-size: 0.92rem; }
  .card {
    background: #fff; border-radius: 14px; padding: 1rem;
    margin-top: 0.9rem;
  }
  .row { display: flex; gap: 0.6rem; }
  button, .ghost {
    font: inherit; border: 0; border-radius: 12px; padding: 0.95rem 0.5rem;
    width: 100%;
  }
  button { background: #1f6b4a; color: #fff; }
  button.off { background: #8a3e2a; }
  button.side { background: #2d3d4f; }
  button.small, .ghost { width: auto; padding: 0.45rem 0.7rem; background: #e7e1d6; color: #1c1915; }
  ul { list-style: none; padding: 0; margin: 0.4rem 0 0; }
  li {
    display: flex; justify-content: space-between; gap: 0.5rem;
    align-items: center; padding: 0.55rem 0; border-top: 1px solid #eee;
  }
  label { display: inline-flex; gap: 0.25rem; align-items: center; margin-right: 0.45rem; }
  select, input[type="time"] {
    font: inherit; padding: 0.55rem; border-radius: 10px; border: 1px solid #d9d2c6;
    background: #fff;
  }
  .days { margin: 0.7rem 0; }
  #msg { min-height: 1.2rem; }
</style>
</head>
<body>
<main>
  <h1>형광등</h1>
  <p id="status" class="muted">상태 확인 중</p>
  <p id="msg" class="muted"></p>
  <div class="card">
    <div class="row">
      <button type="button" id="on">켜기</button>
      <button type="button" class="off" id="off">끄기</button>
    </div>
  </div>
  <div class="card">
    <p>방향 시험. 누른 뒤 2초 후에 가운데로 돌아오고 힘을 뺀다.</p>
    <div class="row">
      <button type="button" class="side" id="left">왼쪽</button>
      <button type="button" class="side" id="right">오른쪽</button>
    </div>
  </div>
  <div class="card">
    <p>예약</p>
    <form id="rule-form">
      <div class="row">
        <select name="action" aria-label="동작">
          <option value="on">켜기</option>
          <option value="off">끄기</option>
        </select>
        <input type="time" name="time" required aria-label="시각">
      </div>
      <div class="days">
        <label><input type="checkbox" name="d" value="1">월</label>
        <label><input type="checkbox" name="d" value="2">화</label>
        <label><input type="checkbox" name="d" value="3">수</label>
        <label><input type="checkbox" name="d" value="4">목</label>
        <label><input type="checkbox" name="d" value="5">금</label>
        <label><input type="checkbox" name="d" value="6">토</label>
        <label><input type="checkbox" name="d" value="0">일</label>
      </div>
      <label><input type="checkbox" name="repeat" value="1" checked>매주 반복</label>
      <p><button type="submit">예약 추가</button></p>
    </form>
    <ul id="rules"></ul>
  </div>
  <p class="muted">집에 도착하면 폰 자동화 앱이 <span id="arrive"></span> 를 연다.</p>
  <p><button type="button" class="small" id="reset-wifi">Wi-Fi 다시 설정</button></p>
</main>
<script>
const days = ["일", "월", "화", "수", "목", "금", "토"];
const statusEl = document.getElementById("status");
const msgEl = document.getElementById("msg");
const rulesEl = document.getElementById("rules");

function say(text) { msgEl.textContent = text || ""; }

async function refresh() {
  const res = await fetch("/api/state");
  const data = await res.json();
  const light = data.light === 1 ? "켜짐" : data.light === 0 ? "꺼짐" : "아직 모름";
  const clock = data.time || "시간 맞추는 중";
  statusEl.textContent = clock + " · 마지막 명령 " + light + (data.busy ? " · 모터 이동 중" : "");
  document.getElementById("arrive").textContent = "http://" + data.ip + "/on";
  rulesEl.innerHTML = "";
  for (const rule of data.rules) {
    const li = document.createElement("li");
    const names = [];
    for (let i = 0; i < 7; i++) if (rule.days & (1 << i)) names.push(days[i]);
    const label = document.createElement("span");
    const hh = String(rule.hour).padStart(2, "0");
    const mm = String(rule.minute).padStart(2, "0");
    label.textContent = hh + ":" + mm + " " + names.join("") + " " + (rule.action === 1 ? "켜기" : "끄기")
      + (rule.repeat ? " · 반복" : " · 한 번") + (rule.enabled ? "" : " · 끝");
    const del = document.createElement("button");
    del.type = "button";
    del.className = "small";
    del.textContent = "삭제";
    del.addEventListener("click", async () => {
      await fetch("/api/rules/delete?id=" + rule.id, { method: "POST" });
      refresh().catch(() => say("목록을 다시 읽지 못했다"));
    });
    li.append(label, del);
    rulesEl.append(li);
  }
}

async function move(to) {
  say("");
  const res = await fetch("/api/move?to=" + to, { method: "POST" });
  const data = await res.json();
  if (data.result === "already") say("이미 그 상태라 누르지 않았다");
  if (data.result === "busy") say("모터가 움직이는 중이라 이어서 실행한다");
  refresh().catch(() => {});
}

document.getElementById("on").addEventListener("click", () => move("on"));
document.getElementById("off").addEventListener("click", () => move("off"));
document.getElementById("left").addEventListener("click", () => move("left"));
document.getElementById("right").addEventListener("click", () => move("right"));

document.getElementById("rule-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  const picked = [...form.querySelectorAll('input[name="d"]:checked')];
  if (!picked.length) { say("요일을 하루 이상 고른다"); return; }
  const body = new URLSearchParams(new FormData(form));
  const res = await fetch("/api/rules", { method: "POST", body });
  if (!res.ok) { say("예약을 추가하지 못했다"); return; }
  form.reset();
  form.repeat.checked = true;
  say("예약을 저장했다");
  refresh().catch(() => {});
});

document.getElementById("reset-wifi").addEventListener("click", async () => {
  if (!confirm("Wi-Fi 설정을 지우고 보드를 다시 시작할까?")) return;
  await fetch("/api/wifi/reset", { method: "POST" });
  say("보드가 설정용 Wi-Fi로 다시 뜬다");
});

refresh().catch(() => say("보드 응답이 없다"));
setInterval(() => refresh().catch(() => {}), 2000);
</script>
</body>
</html>)HTML";

const char SETUP_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Wi-Fi 설정</title>
<style>
  body { margin: 0; font-family: sans-serif; background: #f3f0e8; color: #1c1915; }
  main { max-width: 28rem; margin: 0 auto; padding: 1.25rem; }
  form { display: flex; flex-direction: column; gap: 0.7rem; background: #fff; padding: 1rem; border-radius: 14px; }
  input { font: inherit; padding: 0.7rem; border-radius: 10px; border: 1px solid #d9d2c6; }
  button { font: inherit; border: 0; border-radius: 12px; padding: 0.9rem; background: #1f6b4a; color: #fff; }
</style>
</head>
<body>
<main>
  <h1>집 Wi-Fi 연결</h1>
  <p>2.4GHz 이름과 비밀번호를 입력한다. 저장 후 보드가 다시 시작된다.</p>
  <form method="POST" action="/wifi">
    <input name="ssid" placeholder="Wi-Fi 이름" required maxlength="32" autocomplete="off">
    <input name="password" type="password" placeholder="비밀번호" maxlength="63" autocomplete="off">
    <button type="submit">저장</button>
  </form>
</main>
</body>
</html>)HTML";
