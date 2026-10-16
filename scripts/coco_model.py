Import("env")

from os.path import join
import csv


project_dir = env.subst("$PROJECT_DIR")
build_dir = env.subst("$BUILD_DIR")

source_model = join(
    project_dir,
    "managed_components",
    "espressif__coco_detect",
    "models",
    "s3",
    "coco_detect_yolo11n_320_s8_v1.espdl",
)
pack_script = join(
    project_dir,
    "managed_components",
    "espressif__esp-dl",
    "fbs_loader",
    "pack_espdl_models.py",
)
packed_model = join(build_dir, "coco_detect.espdl")

pack_command = env.VerboseAction(
    '"$PYTHONEXE" "%s" --model_path "%s" --out_file "%s"'
    % (pack_script, source_model, packed_model),
    "Packing COCO Detect model",
)
packed_model_node = env.Command(
    packed_model,
    [source_model, pack_script],
    pack_command,
)

env.Depends("$BUILD_DIR/${PROGNAME}.bin", packed_model_node)
env.Depends("upload", packed_model_node)
# The partition table is the single source of truth for model flash placement.
partition_file = join(project_dir, env.BoardConfig().get("build.partitions", "partitions.csv"))
with open(partition_file, newline="", encoding="utf-8") as table:
    partitions = [row for row in csv.reader(table) if row and not row[0].lstrip().startswith("#")]
model_partition = next((row for row in partitions if row[0].strip() == "coco_det"), None)
if model_partition is None or len(model_partition) < 5 or not model_partition[3].strip():
    raise RuntimeError("coco_det requires an explicit offset in the partition table")
env.Depends(packed_model_node, partition_file)
model_offset = int(model_partition[3].strip(), 0)
size_text = model_partition[4].strip().upper()
multiplier = {"K": 1024, "M": 1024 * 1024}.get(size_text[-1], 1)
model_size = int(size_text[:-1] if multiplier != 1 else size_text, 0) * multiplier

def validate_model_size(target, source, env):
    from os.path import getsize
    if getsize(str(target[0])) > model_size:
        raise RuntimeError("Packed COCO model exceeds the coco_det partition")

env.AddPostAction(packed_model_node, validate_model_size)
env.Append(UPLOADERFLAGS=[hex(model_offset), packed_model])
