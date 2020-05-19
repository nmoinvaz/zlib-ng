from mixpanel import Mixpanel
import os
import argparse

parser = argparse.ArgumentParser(description="MixPanel analytics script")
parser.add_argument("--user_id", help="Unique user id", default=None, action="store", required=True)
parser.add_argument("--event_name", help="Event name", default=None, action="store", required=True)
parser.add_argument("--elapsed_time", help="Elapsed event time", default=None, action="store", required=True)
parser.add_argument("--cmd_result", help="Command line result", default=None, action="store", required=False)
parser.add_argument("--exit_result", help="Exit result", default=None, action="store", required=False)
parser.add_argument("--input_file", help="Input file", default=None, action="store", required=False)
parser.add_argument("--output_file", help="Output file", default=None, action="store", required=False)
args, unknown_args = parser.parse_known_args()

project_token = os.getenv("MIXPANEL_TOKEN")
if not project_token or len(project_token) == 0:
    print("No mixpanel project token found")
    exit(1)

mp = Mixpanel(project_token)

event_info = args.event_name.split("-")
command = event_info[0]
file_info = []

i = 0
# Filename might contain hypens
for i in range(1, len(event_info)):
    # Look for first part containing open mode and
    # compression level
    if len(event_info[i]) > 2:
        file_info.append(event_info[i])
    else:
        break

track_info = {
    "Elapsed Time": args.elapsed_time,
    "Command": command,
    "File": "-".join(file_info),
    "Open Mode": event_info[i][0],
    "Test": " ".join(event_info[i+1:]),
    "Full Command": " ".join(unknown_args)
}

if len(event_info[i]) > 1:
    track_info["Compression Level"] = event_info[i][1]

github_workflow = os.getenv("GITHUB_WORKFLOW")
if github_workflow:
    track_info["GitHub Workflow"] = github_workflow
github_repo = os.getenv("GITHUB_REPO")
if github_repo:
    track_info["GitHub Repository"] = github_repo
github_sha = os.getenv("GITHUB_SHA")
if github_sha:
    track_info["GitHub SHA"] = github_sha
github_ref = os.getenv("GITHUB_REF")
if github_ref:
    track_info["GitHub Ref"] = github_ref
github_action = os.getenv("GITHUB_ACTION")
if github_action:
    track_info["GitHub Action"] = github_action
github_actor = os.getenv("GITHUB_ACTOR")
if github_actor:
    track_info["GitHub Actor"] = github_actor
matrix_name = os.getenv("MATRIX_NAME")
if matrix_name:
    track_info["Matrix Name"] = matrix_name

if args.cmd_result:
    track_info["Command Result"] = args.cmd_result
if args.exit_result:
    track_info["Exit Result"] = args.exit_result

input_file_size = None
if args.input_file and len(args.input_file) > 0:
    track_info["Input File"] = os.path.basename(args.input_file)
    input_file_size = os.path.getsize(args.input_file)
    track_info["Input File Size"] = input_file_size
output_file_size = None
if args.output_file and len(args.output_file) > 0:
    track_info["Output File"] = os.path.basename(args.output_file)
    output_file_size = os.path.getsize(args.output_file)
    track_info["Output File Size"] = output_file_size

if input_file_size > 0 and output_file_size > 0 and args.output_file.endswith(".gz"):
    ratio = (float(output_file_size) / float(input_file_size)) * 100
    track_info["Ratio"] = "{:.2f}".format(ratio)
if input_file_size > 0 and output_file_size > 0 and args.input_file.endswith(".gz"):
    ratio = (float(input_file_size) / float(output_file_size)) * 100
    track_info["Ratio"] = "{:.2f}".format(ratio)

mp.track(args.user_id, args.event_name, track_info)