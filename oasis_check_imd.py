#!/usr/bin/env python3

# www.github.com/hharte/oasis-utils
# Copyright (c) 2021-2025, Howard M. Harte
# SPDX-License-Identifier: MIT

import os
import subprocess
import argparse
import sys

try:
    from termcolor import cprint, colored
except ImportError:
    print("Warning: termcolor library not found. Output will not be colored.")
    print("You can install it with: pip install termcolor")
    # Define dummy functions if termcolor is not available
    def cprint(text, color=None, on_color=None, attrs=None, **kwargs):
        print(text)
    def colored(text, color=None, on_color=None, attrs=None):
        return text

def find_imd_files(directory):
    """
    Recursively finds all .imd files in the given directory.
    Args:
        directory (str): The path to the directory to search.
    Returns:
        list: A list of full paths to .imd files.
    """
    imd_files = []
    if not os.path.isdir(directory):
        cprint(f"Error: Directory '{directory}' not found or is not a directory.", "red")
        return imd_files

    for root, _, files in os.walk(directory):
        for file in files:
            if file.lower().endswith(".imd"):
                imd_files.append(os.path.join(root, file))
    return imd_files

def run_oasis_chkdsk(oasis_chkdsk_cmd, imd_filepath, verbose_chkdsk=False):
    """
    Runs the oasis_chkdsk utility on the given .imd file.
    Args:
        oasis_chkdsk_cmd (str): The command or path to the oasis_chkdsk executable.
        imd_filepath (str): The path to the .imd file.
        verbose_chkdsk (bool): Whether to run oasis_chkdsk in verbose mode.
    Returns:
        tuple: (return_code, stdout, stderr)
    """
    command = [oasis_chkdsk_cmd, imd_filepath]
    if verbose_chkdsk:
        command.append("-v") # Assuming -v is the verbose flag for oasis_chkdsk

    try:
        # MODIFICATION: Added encoding='utf-8' and errors='replace'
        result = subprocess.run(command, capture_output=True, text=True, check=False, encoding='utf-8', errors='replace')
        return result.returncode, result.stdout, result.stderr
    except FileNotFoundError:
        cprint(f"Error: '{oasis_chkdsk_cmd}' executable not found. Please check the path or ensure it's in your system PATH.", "red")
        return -1, "", f"'{oasis_chkdsk_cmd}' not found."
    except Exception as e:
        cprint(f"An unexpected error occurred while trying to run {oasis_chkdsk_cmd}: {e}", "red")
        return -1, "", str(e)

def main():
    parser = argparse.ArgumentParser(
        description="Run oasis_chkdsk on all .imd files in a directory and its subdirectories."
    )
    parser.add_argument(
        "directory",
        help="The directory to search for .imd files."
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Enable verbose output from this script (shows stdout/stderr from oasis_chkdsk on error)."
    )
    parser.add_argument(
        "--chkdsk-verbose",
        action="store_true",
        help="Pass the verbose flag (-v) to the oasis_chkdsk utility itself."
    )
    parser.add_argument(
        "--chkdsk-path",
        default="oasis_chkdsk",
        help="Path to the oasis_chkdsk executable (default: 'oasis_chkdsk', assumes it's in PATH)."
    )

    args = parser.parse_args()

    if not os.path.isdir(args.directory):
        cprint(f"Error: The specified path '{args.directory}' is not a valid directory.", "red")
        sys.exit(1)

    print(f"Searching for .imd files in '{os.path.abspath(args.directory)}'...\n")
    imd_files_found = find_imd_files(args.directory)

    if not imd_files_found:
        print("No .imd files found.")
        sys.exit(0)

    print(f"Found {len(imd_files_found)} .imd file(s). Processing with '{args.chkdsk_path}'...\n")

    overall_success = True
    files_with_errors = 0

    for imd_file in imd_files_found:
        print(f"Checking: {imd_file}")
        return_code, chkdsk_stdout, chkdsk_stderr = run_oasis_chkdsk(args.chkdsk_path, imd_file, args.chkdsk_verbose)

        if return_code == 0:
            cprint("OK", "green")
        else:
            cprint("Errors found.", "red")
            overall_success = False
            files_with_errors += 1
            if args.verbose:
                if chkdsk_stdout:
                    print(colored("--- oasis_chkdsk STDOUT ---", "yellow"))
                    print(chkdsk_stdout.strip())
                    print(colored("---------------------------", "yellow"))
                if chkdsk_stderr:
                    print(colored("--- oasis_chkdsk STDERR ---", "yellow"))
                    print(chkdsk_stderr.strip())
                    print(colored("---------------------------", "yellow"))
        print("-" * 30)

    print("\n--- Summary ---")
    if overall_success:
        cprint(f"All {len(imd_files_found)} .imd file(s) checked out OK.", "green")
    else:
        cprint(f"{files_with_errors} out of {len(imd_files_found)} .imd file(s) reported errors.", "red")
        sys.exit(1)

    sys.exit(0)

if __name__ == "__main__":
    main()
