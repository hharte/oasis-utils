#!/usr/bin/env python3

# www.github.com/hharte/oasis-utils
# Copyright (c) 2021-2025, Howard M. Harte
# SPDX-License-Identifier: MIT

import os
import hashlib
import argparse
import sys

def calculate_md5(filepath, chunk_size=8192):
    """
    Calculates the MD5 hash of a file.

    Args:
        filepath (str): The path to the file.
        chunk_size (int): The size of chunks to read the file in bytes.
                          Helps manage memory for large files.

    Returns:
        str: The hexadecimal MD5 hash string, or None if an error occurs.
    """
    hash_md5 = hashlib.md5()
    try:
        # Open the file in binary read mode
        with open(filepath, "rb") as f:
            # Read the file in chunks to handle potentially large files
            for chunk in iter(lambda: f.read(chunk_size), b""):
                hash_md5.update(chunk)
        # Return the hexadecimal representation of the hash
        return hash_md5.hexdigest()
    except FileNotFoundError:
        print(f"Error: File not found - {filepath}", file=sys.stderr)
        return None
    except IOError as e:
        print(f"Error reading file {filepath}: {e}", file=sys.stderr)
        return None
    except Exception as e:
        # Catch any other unexpected errors during file processing
        print(f"An unexpected error occurred with file {filepath}: {e}", file=sys.stderr)
        return None

def compare_directories(dir1, dir2):
    """
    Compares files with the same name in two directories based on their MD5 checksums.

    Args:
        dir1 (str): Path to the first directory.
        dir2 (str): Path to the second directory.
    """

    # --- 1. Validate Directory Paths ---
    if not os.path.isdir(dir1):
        print(f"Error: Directory not found or is not a valid directory: {dir1}", file=sys.stderr)
        sys.exit(1) # Exit if a directory is invalid
    if not os.path.isdir(dir2):
        print(f"Error: Directory not found or is not a valid directory: {dir2}", file=sys.stderr)
        sys.exit(1)

    print(f"Comparing directories:")
    print(f"  Directory 1: {os.path.abspath(dir1)}")
    print(f"  Directory 2: {os.path.abspath(dir2)}\n")

    # --- 2. List Files in Both Directories ---
    try:
        # Create sets of filenames (only files, not directories) in each directory
        # Using sets makes finding common/unique files efficient
        files1 = {f for f in os.listdir(dir1) if os.path.isfile(os.path.join(dir1, f))}
        files2 = {f for f in os.listdir(dir2) if os.path.isfile(os.path.join(dir2, f))}
    except OSError as e:
        # Handle potential errors listing directory contents (e.g., permission issues)
        print(f"Error listing directory contents: {e}", file=sys.stderr)
        sys.exit(1)

    # --- 3. Identify Common and Unique Files ---
    common_files = files1.intersection(files2) # Files present in both dir1 AND dir2
    files_only_in_dir1 = files1 - files2       # Files present in dir1 BUT NOT dir2
    files_only_in_dir2 = files2 - files1       # Files present in dir2 BUT NOT dir1

    mismatched_files = []
    matched_files = []
    error_files = [] # Keep track of files that couldn't be compared due to errors

    print("--- Comparison Results ---")

    # --- 4. Compare Common Files ---
    if not common_files:
        print("No common files found between the two directories.")
    else:
        print(f"\nComparing {len(common_files)} common file(s):")
        # Sort filenames for consistent output order
        for filename in sorted(list(common_files)):
            filepath1 = os.path.join(dir1, filename)
            filepath2 = os.path.join(dir2, filename)

            print(f"  Comparing '{filename}'...")

            # Calculate MD5 hash for the file in each directory
            md5_1 = calculate_md5(filepath1)
            md5_2 = calculate_md5(filepath2)

            # Check if MD5 calculation was successful for both files
            if md5_1 is not None and md5_2 is not None:
                if md5_1 == md5_2:
                    print(f"    \033[92mMATCH:\033[0m MD5 sums are identical ({md5_1})") # Green text for match
                    matched_files.append(filename)
                else:
                    print(f"    \033[91mMISMATCH:\033[0m MD5 sums differ.") # Red text for mismatch
                    print(f"      '{os.path.basename(dir1)}/{filename}': {md5_1}")
                    print(f"      '{os.path.basename(dir2)}/{filename}': {md5_2}")
                    mismatched_files.append(filename)
            else:
                # If either MD5 calculation failed, report an error for this file
                print(f"    \033[93mERROR:\033[0m Could not compare '{filename}' due to previous file access errors.") # Yellow text for error
                error_files.append(filename)

    # --- 5. Report Summary ---
    print("\n--- Summary ---")
    print(f"Total files compared: {len(common_files)}")
    print(f"  Matching files: {len(matched_files)}")
    print(f"  Mismatched files: {len(mismatched_files)}")
    if error_files:
        print(f"  Files with errors (comparison skipped): {len(error_files)}")

    # List mismatched files if any
    if mismatched_files:
        print("\nFiles with differences:")
        for f in sorted(mismatched_files):
            print(f"  - {f}")

    # List files unique to directory 1
    if files_only_in_dir1:
        print(f"\nFiles only in '{os.path.basename(dir1)}' ({len(files_only_in_dir1)}):")
        for f in sorted(list(files_only_in_dir1)):
            print(f"  - {f}")

    # List files unique to directory 2
    if files_only_in_dir2:
        print(f"\nFiles only in '{os.path.basename(dir2)}' ({len(files_only_in_dir2)}):")
        for f in sorted(list(files_only_in_dir2)):
            print(f"  - {f}")

    # List files that had errors during comparison
    if error_files:
        print("\nFiles skipped due to errors:")
        for f in sorted(error_files):
            print(f"  - {f}")

    print("\nComparison complete.")

# --- Main Execution Block ---
if __name__ == "__main__":
    # Set up command-line argument parsing
    parser = argparse.ArgumentParser(
        description="Compare MD5 checksums of files with the same name in two directories.",
        formatter_class=argparse.RawDescriptionHelpFormatter # Preserve formatting in help message
    )
    # Define positional arguments for the two directories
    parser.add_argument("dir1", help="Path to the first directory.")
    parser.add_argument("dir2", help="Path to the second directory.")

    # Example usage in help message
    parser.epilog = """
Example usage:
  python compare_md5.py /path/to/original/files /path/to/backup/files
  ./compare_md5.py ./source_code ./compiled_output
"""

    # Parse the command-line arguments provided by the user
    args = parser.parse_args()

    # Call the main comparison function with the provided directory paths
    compare_directories(args.dir1, args.dir2)
