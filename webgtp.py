#!/usr/bin/env python3
import http.server
import socketserver
import json
import socket
import threading
import time
import os
import subprocess
import mimetypes
from urllib.parse import urlparse, parse_qs

PORT = 8080
GTP_HOST = '127.0.0.1'
GTP_PORT = 9999

HTML_DIR = os.path.dirname(os.path.abspath(__file__))

class GTPClient:
    def __init__(self):
        self.sock = None
        self.lock = threading.Lock()
        self.id_counter = 1
        
    def connect(self):
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(30)
            self.sock.connect((GTP_HOST, GTP_PORT))
            return True
        except Exception as e:
            print(f"GTP connect error: {e}")
            return False
    
    def send(self, cmd):
        # retry once on a fresh connection, e.g. after the engine was restarted
        resp = self._send(cmd)
        if resp is None:
            resp = self._send(cmd)
        return resp

    def _send(self, cmd):
        with self.lock:
            if not self.sock:
                if not self.connect():
                    return None
            try:
                msg_id = self.id_counter
                self.id_counter += 1
                full_cmd = f"{msg_id} {cmd}\n"
                self.sock.sendall(full_cmd.encode())
                
                response = b""
                while True:
                    chunk = self.sock.recv(4096)
                    if not chunk:
                        break
                    response += chunk
                    if b'\n\n' in response:
                        break
                if not response:
                    # connection closed by the engine; reconnect on the next try
                    self.sock = None
                    return None
                resp_str = response.decode('utf-8', errors='replace')
                return resp_str
            except Exception as e:
                print(f"GTP send error: {e}")
                self.sock = None
                return None

gtp_client = GTPClient()

def gtp(cmd):
    """Send a command, return (success, result text of the first line)."""
    resp = gtp_client.send(cmd)
    if not resp or resp[0] not in '=?':
        return False, ""
    line = resp.split('\n')[0]
    result = line.split(' ', 1)[1].strip() if ' ' in line else ""
    return resp[0] == '=', result

# ---------------------------------------------------------------------------
# Player rank and record, as in igowin: one rank up after a win, one rank down
# after a loss, and the rank decides the opponent level and handicap stones.
# Ranks are ints: -25 = 25 kyu ... -1 = 1 kyu, 1 = 1 dan ... 6 = 6 dan.
# ---------------------------------------------------------------------------

PLAYER_FILE = os.path.join(HTML_DIR, 'igowin-player.json')
GAMES_DIR = os.path.join(HTML_DIR, 'games')
MIN_RANK, MAX_RANK = -25, 6
DEFAULT_RANK = -20

# engine levels and the strength the engine claims for them (ui2g2.h)
ENGINE_LEVELS = [(0, -18), (1, -15), (2, -12), (3, -9), (4, -6), (5, -4), (6, -2)]

def rank_name(r):
    return f"{-r}k" if r < 0 else f"{r}d"

def rank_steps(r):
    """Linear scale without a gap between 1 kyu and 1 dan."""
    return r if r < 0 else r - 1

def rank_up(r):
    return min(MAX_RANK, 1 if r == -1 else r + 1)

def rank_down(r):
    return max(MIN_RANK, -1 if r == 1 else r - 1)

ENGINE_RANK = dict(ENGINE_LEVELS)

# 9x9 igowin style: a separate rank that starts at the maximum handicap.
# One step per rank from 25k to 6d: (level, player color, handicap stones, komi).
# On 9x9 a stone is worth several ranks, so between stones the steps are
# "no komi" (black moves first, 0.5 komi) and an even game.
def _ladder9():
    steps = []
    for level, stones in [(0, [5, 4, 3, 2]), (1, [3, 2]), (2, [2]), (3, [2]),
                          (4, [2]), (5, [2]), (6, [3, 2])]:
        steps += [(level, "black", s, 0.5) for s in stones]
        steps += [(level, "black", 0, 0.5), (level, "black", 0, 6.5)]
    # stronger than the engine: the player takes white and gives stones
    steps += [(6, "white", 0, 0.5)] + [(6, "white", s, 0.5) for s in [2, 3, 4, 5]]
    return steps

LADDER9 = _ladder9()
assert len(LADDER9) == 31   # 25k ... 1k, 1d ... 6d

def rank_index(r):
    return rank_steps(r) - MIN_RANK

def load_player():
    try:
        with open(PLAYER_FILE) as f:
            p = json.load(f)
    except (OSError, ValueError):
        p = {"rank": DEFAULT_RANK, "wins": 0, "losses": 0, "history": []}
    p.setdefault("rank9", MIN_RANK)
    return p

def save_player(p):
    tmp = PLAYER_FILE + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(p, f, indent=1)
    os.replace(tmp, PLAYER_FILE)

def player_info(p):
    return {"rank": p["rank"], "rankName": rank_name(p["rank"]),
            "rank9": p["rank9"], "rank9Name": rank_name(p["rank9"]),
            "wins": p["wins"], "losses": p["losses"], "history": p["history"][-10:]}

def rank_key(size, style):
    """Which rank a game uses: 'rank' (19x19), 'rank9' (9x9 igowin style) or None (unrated)."""
    if size == 19:
        return "rank"
    if size == 9 and style == "igowin":
        return "rank9"
    return None

def game_setup(size, rank, style=None):
    """Opponent level, colors, handicap and komi for a player of this rank."""
    if size == 9 and style == "igowin":
        level, human, handicap, komi = LADDER9[rank_index(rank)]
        return {"size": 9, "rated": True, "style": "igowin", "human": human, "level": level,
                "engineRank": ENGINE_RANK[level], "handicap": handicap, "komi": komi}
    if size != 19:
        # free game: even, unrated, against the strongest level
        return {"size": size, "rated": False, "human": "black", "level": 6,
                "engineRank": -2, "handicap": 0, "komi": 6.5}
    strongest_level, strongest = ENGINE_LEVELS[-1]
    if rank_steps(rank) <= rank_steps(strongest):
        level, engine_rank = next((l, s) for l, s in ENGINE_LEVELS
                                  if rank_steps(s) >= rank_steps(rank))
        human = "black"
        gap = rank_steps(engine_rank) - rank_steps(rank)
    else:
        # stronger than the engine: the player takes white and gives the stones
        level, engine_rank = strongest_level, strongest
        human = "white"
        gap = rank_steps(rank) - rank_steps(strongest)
    gap = min(gap, 9)
    handicap = gap if gap >= 2 else 0
    komi = 6.5 if gap == 0 else 0.5
    return {"size": size, "rated": True, "human": human, "level": level,
            "engineRank": engine_rank, "handicap": handicap, "komi": komi}

current_game = None   # setup of the game in progress, plus "finished"
CURRENT_FILE = os.path.join(HTML_DIR, 'current-game.json')

def save_current():
    """Write the game in progress (setup + moves) so it survives a restart."""
    if current_game is None or current_game["finished"]:
        return
    ok, moves = gtp("igowin_moves")
    if not ok:
        return
    tmp = CURRENT_FILE + '.tmp'
    with open(tmp, 'w') as f:
        json.dump({"setup": current_game, "moves": moves}, f)
    os.replace(tmp, CURRENT_FILE)

def clear_current():
    try:
        os.remove(CURRENT_FILE)
    except OSError:
        pass

def load_current():
    try:
        with open(CURRENT_FILE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None

def setup_engine(setup):
    """Board size, komi, level and handicap stones for a game; returns False without stones."""
    gtp(f"boardsize {setup['size']}")
    gtp("clear_board")
    gtp(f"komi {setup['komi']}")
    gtp(f"level {setup['level']}")
    if setup["handicap"]:
        ok, _ = gtp(f"fixed_handicap {setup['handicap']}")
        return ok
    return True

def resume_game():
    """
    Continue the unfinished game after a page reload or a restart.  If the
    engine still has the game nothing changes; otherwise the saved moves are
    replayed.  Returns the setup, or None when there is no game to continue.
    """
    global current_game
    saved = load_current()
    if saved is None:
        return None
    setup, moves = saved["setup"], saved["moves"]
    ok, engine_moves = gtp("igowin_moves")
    if ok and current_game is not None and engine_moves == moves:
        return current_game
    setup_engine(setup)
    tokens = moves.split()
    pairs = list(zip(tokens[0::2], tokens[1::2]))[setup["handicap"]:]   # stones are placed above
    for color, vertex in pairs:
        ok, _ = gtp(f"play {color} {vertex}")
        if not ok:
            print(f"resume: could not replay {color} {vertex}")
            break
    current_game = setup
    return setup

def start_game(size, rank, style=None):
    global current_game
    setup = game_setup(size, rank, style)
    setup["playerRank"] = rank
    setup["rankKey"] = rank_key(size, style)
    if not setup_engine(setup):
        setup["handicap"] = 0
    setup["finished"] = False
    current_game = setup
    save_current()
    return setup

def to_sgf_point(vertex, size):
    if vertex == 'pass':
        return ''
    letters = 'ABCDEFGHJKLMNOPQRST'
    col = letters.index(vertex[0].upper())
    row = size - int(vertex[1:])
    return 'abcdefghijklmnopqrs'[col] + 'abcdefghijklmnopqrs'[row]

def make_sgf(setup, moves, result):
    """The game as SGF text; result may be empty for a game in progress."""
    size = setup["size"]
    human_rank = rank_name(setup["playerRank"])
    engine_rank = rank_name(setup["engineRank"])
    pb, pw = ("You", "Many Faces of Go") if setup["human"] == "black" else ("Many Faces of Go", "You")
    br, wr = (human_rank, engine_rank) if setup["human"] == "black" else (engine_rank, human_rank)
    tokens = moves.split()
    pairs = list(zip(tokens[0::2], tokens[1::2]))
    hc = setup["handicap"]
    sgf = (f"(;GM[1]FF[4]CA[UTF-8]AP[igowin-web]SZ[{size}]KM[{setup['komi']}]"
           f"HA[{hc}]DT[{time.strftime('%Y-%m-%d')}]PB[{pb}]PW[{pw}]BR[{br}]WR[{wr}]RE[{result}]")
    if hc:
        sgf += "AB" + "".join(f"[{to_sgf_point(v, size)}]" for _, v in pairs[:hc])
        pairs = pairs[hc:]
    for color, vertex in pairs:
        sgf += f";{color.upper()}[{to_sgf_point(vertex, size)}]"
    sgf += ")\n"
    return sgf

def write_sgf(setup, moves, result):
    """Save the game as games/YYYYmmdd-HHMMSS.sgf, return the file name."""
    sgf = make_sgf(setup, moves, result)
    os.makedirs(GAMES_DIR, exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M%S')
    name, n = stamp + '.sgf', 1
    while os.path.exists(os.path.join(GAMES_DIR, name)):
        n += 1
        name = f"{stamp}-{n}.sgf"
    with open(os.path.join(GAMES_DIR, name), 'w') as f:
        f.write(sgf)
    return name

def finish_game(result):
    """result like 'B+3.5' or 'W+R'. Saves SGF, lets the engine learn, updates rank."""
    p = load_player()
    setup = current_game
    if setup is None or setup["finished"]:
        return {"player": player_info(p), "message": ""}
    setup["finished"] = True
    clear_current()
    winner = "black" if result.startswith('B') else "white"
    won = winner == setup["human"]

    _, moves = gtp("igowin_moves")
    sgf_name = write_sgf(setup, moves, result)

    human_rank, engine_rank = setup["playerRank"], setup["engineRank"]
    black_rank, white_rank = ((human_rank, engine_rank) if setup["human"] == "black"
                              else (engine_rank, human_rank))
    learned, _ = gtp(f"igowin_learn {0 if winner == 'black' else 1} {black_rank} {white_rank}")

    key = setup["rankKey"]
    old = setup["playerRank"]
    if key:
        p[key] = rank_up(old) if won else rank_down(old)
    p["wins" if won else "losses"] += 1
    p["history"].append({"date": time.strftime('%Y-%m-%d %H:%M'), "size": setup["size"],
                         "rank": rank_name(old), "result": result, "won": won, "sgf": sgf_name})
    save_player(p)

    message = "You win!" if won else "Computer wins."
    if key and p[key] != old:
        message += f" Rank {rank_name(old)} → {rank_name(p[key])}"
    return {"player": player_info(p), "message": message, "sgf": sgf_name, "learned": learned}

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=HTML_DIR, **kwargs)
    
    def log_message(self, fmt, *args):
        print(f"[{self.log_date_time_string()}] {fmt % args}")
    
    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == '/api/status':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            resp = gtp_client.send("protocol_version")
            status = {"connected": resp is not None and "=" in resp}
            self.wfile.write(json.dumps(status).encode())
            return
        if parsed.path == '/api/player':
            self.send_json(player_info(load_player()))
            return
        if parsed.path == '/api/sgf':
            # the current game as an SGF download (Save button)
            if current_game is None:
                self.send_error(404, "no game")
                return
            _, moves = gtp("igowin_moves")
            body = make_sgf(current_game, moves, "").encode()
            name = time.strftime('igowin-%Y%m%d-%H%M.sgf')
            self.send_response(200)
            self.send_header('Content-Type', 'application/x-go-sgf')
            self.send_header('Content-Disposition', f'attachment; filename="{name}"')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        return super().do_GET()

    def send_json(self, obj):
        self.send_response(200)
        self.send_header('Content-type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(json.dumps(obj).encode())
    
    def do_POST(self):
        parsed = urlparse(self.path)
        content_length = int(self.headers.get('Content-Length', 0))
        post_data = self.rfile.read(content_length)
        
        try:
            data = json.loads(post_data.decode())
        except:
            self.send_error(400)
            return
        
        if parsed.path == '/api/newgame':
            p = load_player()
            size = int(data.get('size', 19))
            style = data.get('style')
            key = rank_key(size, style)
            if key and 'rank' in data:
                # the player may set their rank by hand, e.g. to start at the right level
                p[key] = max(MIN_RANK, min(MAX_RANK, int(data['rank']))) or -1
                save_player(p)
            setup = start_game(size, p[key] if key else p["rank"], style)
            self.send_json({"setup": setup, "player": player_info(p)})
            return
        elif parsed.path == '/api/resume':
            # page load: continue the unfinished game, or start a new one
            p = load_player()
            setup = resume_game()
            resumed = setup is not None
            if not resumed:
                size = int(data.get('size', 19))
                key = rank_key(size, None)
                setup = start_game(size, p[key] if key else p["rank"])
            self.send_json({"setup": setup, "player": player_info(p), "resumed": resumed})
            return
        elif parsed.path == '/api/preview':
            # what a new game would look like, for the New Game dialog
            size = int(data.get('size', 19))
            setup = game_setup(size, int(data.get('rank', DEFAULT_RANK)) or -1, data.get('style'))
            self.send_json({"setup": setup})
            return
        elif parsed.path == '/api/gameover':
            self.send_json(finish_game(str(data.get('result', ''))))
            return
        elif parsed.path == '/api/quit':
            # Quit button: igowin.sh stops engine and web server, releases the wake lock and
            # shows the home screen; it runs on its own, so it survives this server being killed
            self.send_json({"ok": True})
            subprocess.Popen(['./igowin.sh', 'quit'], cwd=os.path.dirname(os.path.abspath(__file__)),
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             start_new_session=True)
            return
        elif parsed.path == '/api/command':
            cmd = data.get('cmd', '')
            resp = gtp_client.send(cmd)
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(json.dumps({"response": resp}).encode())
            return
        elif parsed.path == '/api/gtp':
            cmd = data.get('cmd', '')
            resp = gtp_client.send(cmd)
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            # Parse GTP response: "=<id> <result>" or "?<id> <error>", keep the whole first line
            if resp and resp[0] in '=?':
                result_line = resp.split('\n')[0]
                result = result_line.split(' ', 1)[1] if ' ' in result_line else ""
                success = resp[0] == '='
                self.wfile.write(json.dumps({"success": success, "result": result.strip()}).encode())
                # keep the saved game up to date after every change to the board
                if success and cmd.split()[:1] in (['play'], ['genmove'], ['undo']):
                    save_current()
            else:
                self.wfile.write(json.dumps({"success": False, "result": ""}).encode())
            return
        
        self.send_error(404)
    
    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()

socketserver.TCPServer.allow_reuse_address = True
httpd = socketserver.TCPServer(("127.0.0.1", PORT), Handler)
print(f"Web UI running at http://127.0.0.1:{PORT}")
print(f"Connecting to GTP at {GTP_HOST}:{GTP_PORT}")
gtp_client.connect()
httpd.serve_forever()
