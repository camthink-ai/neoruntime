#include "common.h"

#include <algorithm>
#include <sstream>

namespace aipc::ai_runtime {

static std::string base64_encode(const std::string& data) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        out += tbl[(uint8_t)data[i] >> 2];
        out += tbl[((uint8_t)data[i] & 0x03) << 4 | (uint8_t)data[i+1] >> 4];
        out += tbl[((uint8_t)data[i+1] & 0x0F) << 2 | (uint8_t)data[i+2] >> 6];
        out += tbl[(uint8_t)data[i+2] & 0x3F];
    }
    if (i < data.size()) {
        out += tbl[(uint8_t)data[i] >> 2];
        if (i + 1 < data.size()) {
            out += tbl[((uint8_t)data[i] & 0x03) << 4 | (uint8_t)data[i+1] >> 4];
            out += tbl[((uint8_t)data[i+1] & 0x0F) << 2];
        } else {
            out += tbl[((uint8_t)data[i] & 0x03) << 4];
            out += '=';
        }
        out += '=';
    }
    return out;
}

std::string post_result_to_json(const std::string& stream_id,
                                const std::string& model_id,
                                uint64_t frame_seq,
                                uint64_t timestamp_ns,
                                const HalPostprocessResult& result) {
    std::ostringstream j;
    j << "{\"stream_id\":\"" << stream_id
      << "\",\"model_id\":\"" << model_id
      << "\",\"frame_sequence\":" << frame_seq
      << ",\"timestamp_ns\":" << timestamp_ns;

    if (result.type == HAL_POST_TYPE_DETECTION) {
        auto& det = result.result.detection;
        j << ",\"num_detections\":" << det.num_detections
          << ",\"detections\":[";
        for (uint32_t i = 0; i < det.num_detections; i++) {
            if (i > 0) j << ",";
            auto& d = det.detections[i];
            j << "{\"class_id\":" << d.class_id
              << ",\"label\":\"" << d.label << "\""
              << ",\"confidence\":" << d.confidence
              << ",\"bbox\":[" << d.bbox.x
              << "," << d.bbox.y
              << "," << d.bbox.w
              << "," << d.bbox.h << "]}";
        }
        j << "]";
    } else if (result.type == HAL_POST_TYPE_KEYPOINT) {
        j << ",\"num_keypoint_objects\":" << result.result.keypoint.num_objects;
    } else if (result.type == HAL_POST_TYPE_CLASSIFICATION ||
               result.type == HAL_POST_TYPE_CLIP) {
        auto& cls = result.result.classification;
        j << ",\"num_classes\":" << cls.num_classes
          << ",\"classifications\":[";
        for (uint32_t i = 0; i < cls.num_classes; i++) {
            if (i > 0) j << ",";
            j << "{\"class_id\":" << cls.classes[i].class_id
              << ",\"label\":\"" << cls.classes[i].label << "\""
              << ",\"confidence\":" << cls.classes[i].confidence << "}";
        }
        j << "]";
    } else if (result.type == HAL_POST_TYPE_EMBEDDING) {
        auto& emb = result.result.embedding;
        j << ",\"dim\":" << emb.dim
          << ",\"embedding\":[";
        uint32_t limit = std::min(emb.dim, (uint32_t)16);
        for (uint32_t i = 0; i < limit; i++) {
            if (i > 0) j << ",";
            j << emb.data[i];
        }
        if (emb.dim > 16) j << ",\"...(truncated, total=" << emb.dim << ")\"";
        j << "]";
    } else if (result.type == HAL_POST_TYPE_DEPTH) {
        const auto& d = result.result.depth;
        j << ",\"depth_width\":" << d.width << ",\"depth_height\":" << d.height;
        if (d.depth_m && d.width > 0 && d.height > 0) {
            const size_t n = (size_t)d.width * (size_t)d.height;
            float mn = d.depth_m[0], mx = d.depth_m[0];
            for (size_t i = 1; i < n; i++) {
                if (d.depth_m[i] < mn) mn = d.depth_m[i];
                if (d.depth_m[i] > mx) mx = d.depth_m[i];
            }
            j << ",\"depth_m_min\":" << mn << ",\"depth_m_max\":" << mx;
        }
    } else if (result.type == HAL_POST_TYPE_SEGMENTATION) {
        auto& seg = result.result.segmentation;
        j << ",\"type\":\"segmentation\",\"width\":" << seg.width
          << ",\"height\":" << seg.height
          << ",\"num_classes\":" << seg.num_classes;
        // Per-class RLE masks (base64)
        if (seg.mask_data && seg.width > 0 && seg.height > 0) {
            j << ",\"masks\":[";
            bool first = true;
            for (uint32_t cls = 0; cls < seg.num_classes; cls++) {
                // Simple RLE: scan mask_data for this class_id
                std::string rle;
                const uint32_t total = seg.width * seg.height;
                uint32_t idx = 0;
                while (idx < total) {
                    if (seg.mask_data[idx] == (uint8_t)cls) {
                        uint32_t start = idx;
                        while (idx < total && seg.mask_data[idx] == (uint8_t)cls) idx++;
                        uint32_t len = idx - start;
                        auto enc = [&](uint32_t v) {
                            while (v > 0x7F) { rle += (char)((v & 0x7F) | 0x80); v >>= 7; }
                            rle += (char)v;
                        };
                        enc(start);
                        enc(len);
                    } else { idx++; }
                }
                if (rle.empty()) continue;
                if (!first) j << ","; first = false;
                // Compute bbox
                uint32_t x0 = seg.width, y0 = seg.height, x1 = 0, y1 = 0;
                for (uint32_t r = 0; r < seg.height; r++)
                    for (uint32_t c = 0; c < seg.width; c++)
                        if (seg.mask_data[r * seg.width + c] == (uint8_t)cls) {
                            if (c < x0) x0 = c; if (c > x1) x1 = c;
                            if (r < y0) y0 = r; if (r > y1) y1 = r;
                        }
                j << "{\"class_id\":" << cls
                  << ",\"bbox\":[" << (float)x0/seg.width << "," << (float)y0/seg.height
                  << "," << (float)(x1-x0+1)/seg.width << "," << (float)(y1-y0+1)/seg.height << "]"
                  << ",\"mask_rle\":\"" << base64_encode(rle) << "\""
                  << ",\"mask_width\":" << seg.width
                  << ",\"mask_height\":" << seg.height << "}";
            }
            j << "]";
        }
    } else if (result.type == HAL_POST_TYPE_OCR_RECOGNITION ||
               result.type == HAL_POST_TYPE_OCR_DETECTION) {
        auto& ocr = result.result.ocr;
        j << ",\"type\":\"ocr\",\"num_lines\":" << ocr.num_lines
          << ",\"ocr_lines\":[";
        for (uint32_t i = 0; i < ocr.num_lines; i++) {
            if (i > 0) j << ",";
            auto& line = ocr.lines[i];
            j << "{\"text\":\"" << line.text << "\""
              << ",\"confidence\":" << line.confidence
              << ",\"bbox\":[" << line.bbox.x
              << "," << line.bbox.y
              << "," << line.bbox.w
              << "," << line.bbox.h << "]}";
        }
        j << "]";
    }

    j << "}";
    return j.str();
}

}  // namespace aipc::ai_runtime

// ─── Protobuf variant ─────────────────────────────────────────────────────────
#include "inference.pb.h"

namespace aipc::ai_runtime {

std::string post_result_pb_to_json(const std::string& stream_id,
                                   const std::string& model_id,
                                   uint64_t frame_seq,
                                   uint64_t timestamp_ns,
                                   const aipc::inference::PostResult& result) {
    std::ostringstream j;
    j << "{\"stream_id\":\"" << stream_id
      << "\",\"model_id\":\"" << model_id
      << "\",\"frame_sequence\":" << frame_seq
      << ",\"timestamp_ns\":" << timestamp_ns;

    // Detections
    if (result.detections_size() > 0) {
        j << ",\"num_detections\":" << result.detections_size()
          << ",\"detections\":[";
        for (int i = 0; i < result.detections_size(); i++) {
            if (i > 0) j << ",";
            auto& d = result.detections(i);
            j << "{\"class_id\":" << d.class_id()
              << ",\"label\":\"" << d.label() << "\""
              << ",\"confidence\":" << d.confidence()
              << ",\"bbox\":[" << d.bbox().x()
              << "," << d.bbox().y()
              << "," << d.bbox().w()
              << "," << d.bbox().h() << "]}";
        }
        j << "]";
    }

    // Classifications
    if (result.classifications_size() > 0) {
        j << ",\"num_classes\":" << result.classifications_size()
          << ",\"classifications\":[";
        for (int i = 0; i < result.classifications_size(); i++) {
            if (i > 0) j << ",";
            auto& c = result.classifications(i);
            j << "{\"class_id\":" << c.class_id()
              << ",\"label\":\"" << c.label() << "\""
              << ",\"confidence\":" << c.confidence() << "}";
        }
        j << "]";
    }

    // Landmarks
    if (result.landmarks_size() > 0) {
        j << ",\"num_keypoint_objects\":" << result.landmarks_size();
    }

    // Segmentation masks
    if (result.masks_size() > 0) {
        j << ",\"type\":\"segmentation\",\"masks\":[";
        for (int i = 0; i < result.masks_size(); i++) {
            if (i > 0) j << ",";
            auto& m = result.masks(i);
            j << "{\"class_id\":" << m.class_id()
              << ",\"bbox\":[" << m.bbox().x() << "," << m.bbox().y()
              << "," << m.bbox().w() << "," << m.bbox().h() << "]"
              << ",\"mask_rle\":\"" << base64_encode(m.mask_rle()) << "\""
              << ",\"mask_width\":" << m.mask_width()
              << ",\"mask_height\":" << m.mask_height() << "}";
        }
        j << "]";
    }

    // OCR lines
    if (result.ocr_lines_size() > 0) {
        j << ",\"type\":\"ocr\",\"ocr_lines\":[";
        for (int i = 0; i < result.ocr_lines_size(); i++) {
            if (i > 0) j << ",";
            auto& line = result.ocr_lines(i);
            j << "{\"text\":\"" << line.text() << "\""
              << ",\"confidence\":" << line.confidence()
              << ",\"bbox\":[" << line.bbox().x()
              << "," << line.bbox().y()
              << "," << line.bbox().w()
              << "," << line.bbox().h() << "]}";
        }
        j << "]";
    }

    j << "}";
    return j.str();
}

}  // namespace aipc::ai_runtime
