import socket
import threading
import time
import struct
import os
import shutil
import numpy as np
import cv2
from ipaddress import IPv4Network
from queue import Queue
from deepface import DeepFace

# ================= CẤU HÌNH =================
PORT = 8888
NETWORK = "192.168.0.103/24"
MAX_THREADS = 50
DATASET_PATH = "dataset"
EMBEDDING_FILE = "embeddings.npy"

os.makedirs(DATASET_PATH, exist_ok=True)
frame_lock = threading.Lock()

# ================= BIẾN TOÀN CỤC =================
client = None
connected = False
frame_global = None
frame_count_session = 0
is_busy = False
db = []

# === PHẦN THÊM DUY NHẤT: KIỂM TRA MẤT KẾT NỐI ===
def check_connection():
    global connected, client
    while True:
        if connected and client:
            try:
                # Gửi thử 1 byte rác để kiểm tra socket còn sống không
                client.sendall(b'\x00')
            except:
                print("❌ Mất kết nối với ESP32!")
                connected = False
        time.sleep(3)

threading.Thread(target=check_connection, daemon=True).start()
# ===============================================

# ================= HÀM HỖ TRỢ AI =================
def load_db():
    global db
    if os.path.exists(EMBEDDING_FILE):
        try:
            db = list(np.load(EMBEDDING_FILE, allow_pickle=True))
            print(f"✅ Đã tải {len(db)} khuôn mặt.")
        except:
            db = []
    else:
        db = []


def build_db():
    global db
    new_db = []
    print("⏳ Đang cập nhật Database (Embeddings)...")
    if not os.path.exists(DATASET_PATH):
        os.makedirs(DATASET_PATH)
    for person in os.listdir(DATASET_PATH):
        p_dir = os.path.join(DATASET_PATH, person)
        if not os.path.isdir(p_dir): continue
        for img in os.listdir(p_dir):
            try:
                res = DeepFace.represent(os.path.join(p_dir, img), model_name="SFace",
                                         detector_backend="opencv", enforce_detection=False)
                if res:
                    new_db.append({"name": person, "embedding": res[0]["embedding"]})
            except:
                pass
    db = new_db
    np.save(EMBEDDING_FILE, db)
    print(f"✅ Cập nhật xong! Tổng {len(db)} vector.")


# ================= TÁC VỤ XÓA TẤT CẢ (KHI NHẤN PHÍM 7) =================
def run_delete_all():
    global db, is_busy
    is_busy = True  # Chặn các tác vụ nhận dạng khác trong lúc xóa
    print("⚠️ LỆNH TỪ ESP32: ĐANG XÓA TOÀN BỘ KHUÔN MẶT TRÊN SERVER...")
    try:
        # 1. Xóa thư mục dataset và tạo mới
        if os.path.exists(DATASET_PATH):
            shutil.rmtree(DATASET_PATH)
            time.sleep(0.5)
            os.makedirs(DATASET_PATH)

        # 2. Xóa file embeddings
        if os.path.exists(EMBEDDING_FILE):
            os.remove(EMBEDDING_FILE)

        # 3. Làm trống mảng db trong RAM
        db = []
        print("🗑️ ĐÃ XÓA SẠCH DỮ LIỆU TRÊN SERVER THÀNH CÔNG!")
    except Exception as e:
        print(f"❌ Lỗi khi xóa dữ liệu: {e}")
    finally:
        is_busy = False


# ================= TÁC VỤ NHẬN DẠNG =================
def run_recognition():
    global is_busy, frame_global, client, db, frame_count_session
    if is_busy: return
    is_busy = True
    with frame_lock:
        frame_count_session = 0
    found_id = "N"
    start_time = time.time()
    win_name = "AI_RECOGNITION"
    window_created = False
    while time.time() - start_time < 10:
        try:
            client.sendall(b"G\n")
        except:
            break
        if frame_count_session >= 4:
            local_frame = None
            with frame_lock:
                if frame_global is not None: local_frame = frame_global.copy()
            if local_frame is not None:
                cv2.imshow(win_name, local_frame)
                window_created = True
                cv2.waitKey(1)
                try:
                    DeepFace.extract_faces(local_frame, detector_backend="opencv", enforce_detection=True)
                    client.sendall(b"1TR\n")
                    res = DeepFace.represent(local_frame, model_name="SFace",
                                             detector_backend="opencv", enforce_detection=False)[0]["embedding"]
                    for item in db:
                        score = np.dot(res, item["embedding"]) / (
                                    np.linalg.norm(res) * np.linalg.norm(item["embedding"]))
                        if score > 0.70:
                            found_id = item["name"]
                            break
                    if found_id != "N": break
                except:
                    try:
                        client.sendall(b"1NF\n")
                    except:
                        pass
        time.sleep(0.05)
    try:
        client.sendall(f"1D{found_id}\n".encode())
    except:
        pass
    if window_created:
        try:
            cv2.destroyWindow(win_name); cv2.waitKey(1)
        except:
            pass
    is_busy = False


# ================= TÁC VỤ THÊM MẶT =================
def run_enroll(id_save):
    global is_busy, frame_global, client, frame_count_session
    if is_busy: return
    is_busy = True
    win_name = "AI_ENROLL"
    window_created = False
    path = os.path.join(DATASET_PATH, id_save)
    if os.path.exists(path): shutil.rmtree(path)
    os.makedirs(path)
    captured = 0
    start_enroll_time = time.time()
    while captured < 5:
        if time.time() - start_enroll_time > 20: break
        with frame_lock:
            frame_count_session = 0
        while True:
            if time.time() - start_enroll_time > 20: break
            try:
                client.sendall(b"G\n")
            except:
                break
            if frame_count_session >= 4:
                local_frame = None
                with frame_lock:
                    if frame_global is not None: local_frame = frame_global.copy()
                if local_frame is not None:
                    try:
                        DeepFace.extract_faces(local_frame, detector_backend="opencv", enforce_detection=True)
                        client.sendall(b"1TRS\n")
                        cv2.imshow(win_name, local_frame)
                        window_created = True
                        cv2.waitKey(1)
                        cv2.imwrite(f"{path}/{captured}.jpg", local_frame)
                        captured += 1
                        start_enroll_time = time.time()
                        break
                    except:
                        try:
                            client.sendall(b"1NFS\n")
                        except:
                            pass
                        with frame_lock:
                            frame_count_session = 0
            time.sleep(0.1)
        time.sleep(0.5)
    if captured == 5:
        build_db()
        try:
            client.sendall(f"1O{id_save}\n".encode())
        except:
            pass
    else:
        try:
            client.sendall(b"1S\n")
        except:
            pass
    if window_created:
        try:
            cv2.destroyWindow(win_name); cv2.waitKey(1)
        except:
            pass
    is_busy = False


# ================= XỬ LÝ XÓA ĐƠN LẺ =================
def run_delete(id_del):
    path = os.path.join(DATASET_PATH, id_del)
    if os.path.exists(path):
        shutil.rmtree(path)
        build_db()
        print(f"🗑️ Đã xóa ID: {id_del}")


# ================= KẾT NỐI & QUÉT MẠNG =================
def scan_esp_fast():
    q = Queue()

    def check(ip):
        try:
            s = socket.socket();
            s.settimeout(0.3)
            s.connect((str(ip), PORT));
            q.put(str(ip));
            s.close()
        except:
            pass

    threads = []
    for ip in IPv4Network(NETWORK, strict=False):
        t = threading.Thread(target=check, args=(ip,));
        t.start();
        threads.append(t)
        if len(threads) >= MAX_THREADS:
            for th in threads: th.join()
            threads = []
    return q.get() if not q.empty() else None


def receiver_loop():
    global connected, frame_global, is_busy, client, frame_count_session
    buffer = b""
    while True:
        if connected:
            try:
                chunk = client.recv(8192)
                if not chunk: connected = False; continue
                buffer += chunk
                while len(buffer) >= 4:
                    if buffer[0] in [49, 50]:  # '1' hoặc '2'
                        pos = buffer.find(b'\n')
                        if pos != -1:
                            line = buffer[:pos].decode('utf-8', 'ignore').strip()
                            buffer = buffer[pos + 1:]

                            # LOGIC ĐIỀU KHIỂN
                            if line == "1B":
                                threading.Thread(target=run_recognition, daemon=True).start()
                            elif line.startswith("1T"):
                                threading.Thread(target=run_enroll, args=(line[2:],), daemon=True).start()
                            elif line.startswith("2F"):
                                threading.Thread(target=run_delete, args=(line[2:],), daemon=True).start()
                            elif line == "2RESET":  # PHẢN HỒI LỆNH RESET TỪ PHÍM 7
                                threading.Thread(target=run_delete_all, daemon=True).start()
                        else:
                            break
                    else:
                        try:
                            size = struct.unpack("<I", buffer[:4])[0]
                            if 1000 < size < 150000:
                                if len(buffer) < size + 4: break
                                img_data = buffer[4:4 + size]
                                buffer = buffer[4 + size:]
                                nparr = np.frombuffer(img_data, np.uint8)
                                img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
                                if img is not None:
                                    with frame_lock:
                                        frame_global = cv2.rotate(img, cv2.ROTATE_180)
                                        frame_count_session += 1
                            else:
                                buffer = buffer[1:]
                        except:
                            buffer = buffer[1:]
            except:
                connected = False
                cv2.destroyAllWindows()
        else:
            print("🔍 Đang tìm ESP32...")
            ip = scan_esp_fast()
            if ip:
                try:
                    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    client.connect((ip, PORT))
                    connected = True
                    print(f"🚀 Connected: {ip}")
                except:
                    pass
            time.sleep(2)


if __name__ == "__main__":
    print("--- SERVER AI ACTIVE ---")
    load_db()
    receiver_loop() 
