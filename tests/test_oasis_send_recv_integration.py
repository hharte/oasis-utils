#!/usr/bin/env python3

# www.github.com/hharte/oasis-utils
# Copyright (c) 2021-2025, Howard M. Harte
# SPDX-License-Identifier: MIT

import argparse
import os
import pathlib
import shlex
import shutil
import signal
import subprocess
import sys
import time
import stat # For checking if a path is a character device

# --- Configuration and Variables ---
TEMP_BASE_DIR_NAME = f"test_send_recv_temp_{os.getpid()}"
EXTRACTED_DIR_NAME = "extracted_files"
RECEIVED_DIR_NAME = "received_files"
SOCAT_PID_FILE_NAME = "socat.pid" # Retained for original logic, but may not be used if ports are direct
PTY_A_SYMLINK_NAME = "pty_a_symlink" # Used by socat
PTY_B_SYMLINK_NAME = "pty_b_symlink" # Used by socat

socat_process = None
# These will store the actual device paths to be used by send/recv
# They can be COM ports or PTYs from socat
port_for_recv = None # Equivalent to PTY_A or com_port_recv
port_for_send = None # Equivalent to PTY_B or com_port_send
temp_base_dir_path = None
use_socat_globally = False # To track if socat was used, for cleanup

# --- Helper Functions ---
def cleanup():
    """Cleans up temporary directories and processes."""
    global socat_process, port_for_recv, port_for_send, temp_base_dir_path, use_socat_globally
    print("Cleaning up...")

    if use_socat_globally and socat_process:
        print(f"Stopping socat (PID: {socat_process.pid})...")
        socat_process.terminate()
        try:
            socat_process.wait(timeout=0.5)
        except subprocess.TimeoutExpired:
            print(f"Forcing socat (PID: {socat_process.pid}) to stop...")
            socat_process.kill()
        socat_process = None
        # No need for fallback PTY cleanup if we track socat_process directly

    if temp_base_dir_path and temp_base_dir_path.exists():
        print(f"Removing temporary directory: {temp_base_dir_path}")
        shutil.rmtree(temp_base_dir_path, ignore_errors=True)

    print("Cleanup complete.")


def check_command(cmd_path_str: str, cmd_name: str) -> str:
    """
    Checks if a command is available.
    Args:
        cmd_path_str: The command or path to the executable.
        cmd_name: The conceptual name of the command for error messages.
    Returns:
        The resolved path to the command.
    Raises:
        SystemExit: If the command is not found or not executable.
    """
    cmd_path = pathlib.Path(cmd_path_str)
    resolved_path_str = shutil.which(str(cmd_path))

    if resolved_path_str:
        resolved_path = pathlib.Path(resolved_path_str)
        if resolved_path.is_file() and os.access(resolved_path, os.X_OK):
            print(f"Found {cmd_name} (using: {resolved_path})")
            return str(resolved_path)
        else:
            print(f"Error: {cmd_name} found at '{resolved_path}' but is not executable or not a file.")
            sys.exit(1)
    else:
        if cmd_path.is_file() and os.access(cmd_path, os.X_OK):
             print(f"Found {cmd_name} (using direct path: {cmd_path})")
             return str(cmd_path)

        print(f"Error: {cmd_name} command not found or not executable ('{cmd_path_str}'). Ensure it's in PATH or provide a full path.")
        sys.exit(1)

def main_logic(
    oasis_disk_util_exec: str,
    oasis_send_exec: str,
    oasis_recv_exec: str,
    disk_image: str,
    compare_md5_exec: str,
    com_port_recv_arg: str = None,
    com_port_send_arg: str = None
):
    """
    Main logic for the integration test.
    """
    global socat_process, port_for_recv, port_for_send, temp_base_dir_path, use_socat_globally

    print("Starting OASIS Send/Receive Integration Test (Python Version)...")

    # --- 0. Initial Checks & Port Strategy ---
    use_socat_globally = False # Default to not using socat

    if com_port_recv_arg and com_port_send_arg:
        print(f"Using provided COM ports: RECV on '{com_port_recv_arg}', SEND on '{com_port_send_arg}'.")
        port_for_recv = com_port_recv_arg
        port_for_send = com_port_send_arg
        # On Windows, we MUST use provided COM ports if we reach here.
        # On Linux/macOS, if ports are provided, we use them and skip socat.
    elif com_port_recv_arg or com_port_send_arg: # Only one is provided
        print("Error: If using COM ports, both --com-port-recv and --com-port-send must be specified.")
        sys.exit(1)
    else: # No COM ports provided
        if sys.platform == "win32":
            print("Error: On Windows, virtual COM port pair (e.g., COM1 COM2) must be specified using "
                  "--com-port-recv and --com-port-send options.")
            print("       These ports should be connected by a virtual null-modem cable (e.g., using com0com).")
            sys.exit(1)
        else: # Linux or macOS, and no COM ports provided
            print("COM ports not specified. Will attempt to use socat for virtual serial port loopback.")
            use_socat_globally = True


    # --- 1. Tool Checks ---
    try:
        if use_socat_globally:
            socat_cmd = check_command("socat", "socat")
        else:
            socat_cmd = None # Not needed, but check_command would fail if called with empty string
        oasis_disk_util_cmd = check_command(oasis_disk_util_exec, "oasis_disk_util")
        oasis_send_cmd = check_command(oasis_send_exec, "oasis_send")
        oasis_recv_cmd = check_command(oasis_recv_exec, "oasis_recv")
        compare_md5_cmd = check_command(compare_md5_exec, "compare_md5.py")
    except SystemExit:
        return # Error message printed by check_command

    disk_image_path = pathlib.Path(disk_image)
    if not disk_image_path.is_file():
        print(f"Error: Disk image '{disk_image}' not found.")
        sys.exit(1)
    print(f"Using disk image: {disk_image_path}")

    # --- 2. Setup Temporary Directories ---
    current_script_dir = pathlib.Path(os.getcwd()) # Use current working directory for temp output
    temp_base_dir_path = current_script_dir / TEMP_BASE_DIR_NAME

    extracted_dir = temp_base_dir_path / EXTRACTED_DIR_NAME
    received_dir = temp_base_dir_path / RECEIVED_DIR_NAME

    print("Creating temporary directories...")
    if temp_base_dir_path.exists():
        shutil.rmtree(temp_base_dir_path, ignore_errors=True)
    extracted_dir.mkdir(parents=True, exist_ok=True)
    received_dir.mkdir(parents=True, exist_ok=True)

    # --- 3. Socat Setup (if applicable) ---
    if use_socat_globally:
        pty_a_symlink_path = temp_base_dir_path / PTY_A_SYMLINK_NAME
        pty_b_symlink_path = temp_base_dir_path / PTY_B_SYMLINK_NAME
        print("Starting socat to create virtual serial port loopback...")
        socat_cmd_list = [
            socat_cmd,
            "-d", "-d", # Double -d for more verbose output from socat
            f"pty,raw,echo=0,link={pty_a_symlink_path}",
            f"pty,raw,echo=0,link={pty_b_symlink_path}"
        ]
        try:
            # Start socat and allow it to run in the background
            socat_process = subprocess.Popen(socat_cmd_list, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            print(f"socat started with PID: {socat_process.pid}")
        except Exception as e:
            print(f"Error starting socat: {e}")
            sys.exit(1) # Exit here as cleanup() might not be in a finally block yet.

        print("Waiting for PTY devices to be ready...")
        max_pty_wait_seconds = 15
        local_pty_a_path_socat = None
        local_pty_b_path_socat = None

        for i in range(max_pty_wait_seconds):
            if local_pty_a_path_socat is None and pty_a_symlink_path.is_symlink():
                try:
                    temp_path_a = pty_a_symlink_path.resolve(strict=True)
                    if temp_path_a.exists() and stat.S_ISCHR(temp_path_a.stat().st_mode):
                        local_pty_a_path_socat = temp_path_a
                except (FileNotFoundError, RuntimeError):
                    pass

            if local_pty_b_path_socat is None and pty_b_symlink_path.is_symlink():
                try:
                    temp_path_b = pty_b_symlink_path.resolve(strict=True)
                    if temp_path_b.exists() and stat.S_ISCHR(temp_path_b.stat().st_mode):
                        local_pty_b_path_socat = temp_path_b
                except (FileNotFoundError, RuntimeError):
                    pass

            if local_pty_a_path_socat and local_pty_b_path_socat:
                break
            print(f"PTYs not fully ready yet. Symlink A: {'YES' if pty_a_symlink_path.is_symlink() else 'NO'}, "
                  f"Symlink B: {'YES' if pty_b_symlink_path.is_symlink() else 'NO'}. "
                  f"Resolved A: '{local_pty_a_path_socat if local_pty_a_path_socat else 'not resolved'}', "
                  f"Resolved B: '{local_pty_b_path_socat if local_pty_b_path_socat else 'not resolved'}'. "
                  f"Waiting... ({i + 1}/{max_pty_wait_seconds})")
            time.sleep(1)
        else: # Loop finished without break
            print("Error: Timeout or failure waiting for socat PTY devices to become ready.")
            # ... (extended error reporting from previous version) ...
            if socat_process and socat_process.poll() is None:
                socat_process.kill() # Kill socat if it started but PTYs didn't appear
            sys.exit(1)

        port_for_recv = str(local_pty_a_path_socat)
        port_for_send = str(local_pty_b_path_socat)
        print(f"socat PTY A (for recv): {port_for_recv}")
        print(f"socat PTY B (for send): {port_for_send}")
        time.sleep(0.5)

    # --- 4. Extraction ---
    print(f"Extracting files from disk image '{disk_image_path}' to '{extracted_dir}'...")
    extract_cmd = [oasis_disk_util_cmd, str(disk_image_path), "extract", str(extracted_dir), "-u", "*"]
    try:
        subprocess.run(extract_cmd, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Error: oasis_disk_util failed to extract files. Exit code: {e.returncode}")
        sys.exit(1)

    num_extracted = len(list(p for p in extracted_dir.iterdir() if p.is_file()))
    print(f"File extraction complete. {num_extracted} files extracted.")

    if num_extracted == 0:
        print("No files were extracted from the disk image. Test cannot proceed with send/receive.")
        print("Test finished: No files to transfer.")
        sys.exit(0)

    # --- 5. Send and Receive Block ---
    print("Starting send and receive process for all extracted files...")
    print("-----------------------------------------------------")
    print(f"  Sending from {port_for_send}, Receiving on {port_for_recv}")

    recv_process_obj = None
    try:
        print(f"  Starting oasis_recv on {port_for_recv} to receive into {received_dir} ...")
        recv_cmd_list = [oasis_recv_cmd, port_for_recv, str(received_dir), "--pacing-packet", "10"]
        recv_process_obj = subprocess.Popen(recv_cmd_list)
        print(f"  oasis_recv PID: {recv_process_obj.pid}")

        time.sleep(1)

        extracted_files_to_send = [str(f) for f in extracted_dir.iterdir() if f.is_file()]
        if not extracted_files_to_send:
             print("No files in extracted directory to send. This should not happen if num_extracted > 0.")
             if recv_process_obj.poll() is None: recv_process_obj.terminate()
             sys.exit(1)

        print(f"  Sending all files from '{extracted_dir}' with oasis_send on {port_for_send}...")
        send_cmd_list = [oasis_send_cmd, port_for_send, "--pacing-packet", "10"] + extracted_files_to_send
        send_result = subprocess.run(send_cmd_list)

        if send_result.returncode != 0:
            print(f"Error: oasis_send failed (exit code {send_result.returncode}).")
            if recv_process_obj.poll() is None: recv_process_obj.terminate()
            sys.exit(1)
        print("  oasis_send completed sending all files.")

        print(f"  Waiting for oasis_recv (PID: {recv_process_obj.pid}) to complete...")
        recv_process_obj.wait()
        if recv_process_obj.returncode != 0:
            print(f"Error: oasis_recv failed or was interrupted (Exit code: {recv_process_obj.returncode}).")
            sys.exit(1)
        print("  oasis_recv completed.")

    except Exception as e:
        print(f"An error occurred during send/receive: {e}")
        if recv_process_obj and recv_process_obj.poll() is None:
            print(f"Terminating oasis_recv (PID: {recv_process_obj.pid}) due to error.")
            recv_process_obj.terminate()
        sys.exit(1)

    print("-----------------------------------------------------")
    print("Send and receive process complete.")

    # --- 6. Comparison ---
    print("Comparing extracted files with received files using MD5 checksums...")
    # Ensure compare_md5_exec is executable directly or via python3
    compare_cmd_list = []
    if compare_md5_exec.endswith(".py"):
        python_exe = shutil.which("python3") or shutil.which("python")
        if not python_exe:
            print("Error: Python interpreter (python3 or python) not found for compare_md5.py.")
            sys.exit(1)
        compare_cmd_list = [python_exe, compare_md5_cmd, str(extracted_dir), str(received_dir)]
    else: # Assume it's directly executable
        compare_cmd_list = [compare_md5_cmd, str(extracted_dir), str(received_dir)]

    try:
        compare_run_result = subprocess.run(compare_cmd_list, check=True, capture_output=True, text=True)
        print(compare_run_result.stdout)
        if compare_run_result.stderr:
             print(f"Comparison script stderr:\n{compare_run_result.stderr}", file=sys.stderr)
    except subprocess.CalledProcessError as e:
        print(f"Error: File comparison failed. (Exit code: {e.returncode})")
        print(f"Stdout:\n{e.stdout}")
        print(f"Stderr:\n{e.stderr}")
        sys.exit(1)

    print("All files successfully transferred and verified!")
    print("Integration test PASSED.")
    sys.exit(0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Python version of OASIS Send/Receive Integration Test."
    )
    parser.add_argument("oasis_disk_util_exec", help="Path to the oasis_disk_util executable.")
    parser.add_argument("oasis_send_exec", help="Path to the oasis_send executable.")
    parser.add_argument("oasis_recv_exec", help="Path to the oasis_recv executable.")
    parser.add_argument("disk_image", help="Path to the OASIS disk image file.")
    parser.add_argument("compare_md5_exec", help="Path to the compare_md5.py script or executable.")
    parser.add_argument("--com-port-recv", help="COM port for receiving (e.g., COM1 or /dev/ttyUSB0). If set, --com-port-send must also be set. Skips socat.", default=None)
    parser.add_argument("--com-port-send", help="COM port for sending (e.g., COM2 or /dev/ttyUSB1). If set, --com-port-recv must also be set. Skips socat.", default=None)

    script_args = parser.parse_args()

    try:
        main_logic(
            script_args.oasis_disk_util_exec,
            script_args.oasis_send_exec,
            script_args.oasis_recv_exec,
            script_args.disk_image,
            script_args.compare_md5_exec,
            script_args.com_port_recv,
            script_args.com_port_send
        )
    except SystemExit as e:
        # main_logic calls sys.exit, so this will catch it.
        # Cleanup is called within main_logic's error paths or at the end.
        if e.code != 0:
            print(f"Script exiting with code {e.code} due to error in main_logic.")
        # No need to call cleanup() here if main_logic handles it internally on all paths.
        # However, for robustness, if main_logic doesn't have a try/finally itself:
        if temp_base_dir_path: # Check if temp_base_dir_path was initialized
             cleanup()
        sys.exit(e.code)
    except KeyboardInterrupt:
        print("\nTest interrupted by user.")
        cleanup() # Ensure cleanup on Ctrl+C
        sys.exit(1)
    except Exception as e:
        print(f"An unexpected error occurred in the main execution block: {e}")
        import traceback
        traceback.print_exc()
        cleanup() # Ensure cleanup on other unexpected errors
        sys.exit(1)
    finally:
        # Fallback cleanup, though main_logic should ideally handle its own.
        # If main_logic always calls cleanup on exit/error, this might be redundant
        # or could be removed if main_logic's cleanup is comprehensive.
        # For now, keeping it as a safety net.
        if temp_base_dir_path and temp_base_dir_path.exists():
             # print("Final cleanup call from __main__'s finally block.") # For debugging
             cleanup()
        pass
