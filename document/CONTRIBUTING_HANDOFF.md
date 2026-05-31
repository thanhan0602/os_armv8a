# Quy trình cập nhật handoff/doc subsystem

## Khi nào cần cập nhật handoff?
- Khi thêm/chỉnh sửa tính năng lớn ở một subsystem (MMU, heap, loader, shell, ...)
- Khi sửa invariant, thay đổi file chính, hoặc gặp bug/phát hiện pitfall mới
- Khi hoàn thành milestone, hoặc có bước verify mới

## Cách cập nhật handoff
1. Mở file handoff subsystem tương ứng trong `document/` (ví dụ: `handoff_mmu.md`)
2. Cập nhật các mục:
   - **Mục tiêu**: Nếu phạm vi subsystem thay đổi
   - **Trạng thái hiện tại**: Thêm/cập nhật tính năng, milestone, known issues
   - **File chính**: Nếu thêm/xóa file code chính
   - **Invariant/Assumption**: Nếu có bất biến mới hoặc thay đổi
   - **Lệnh verify nhanh**: Nếu có cách test mới
   - **Pitfall/Debug note**: Nếu gặp bug, trap, kinh nghiệm debug mới
   - **Next steps**: Nếu có TODO, hướng phát triển mới
3. Ghi chú ngắn gọn, tập trung vào thông tin giúp agent/contributor truy vấn nhanh.
4. Không cần log lịch sử chi tiết, chỉ cập nhật trạng thái hiện tại và delta quan trọng.

## Khi thêm subsystem mới
- Copy `handoff_subsystem_template.md` thành file mới, điền các mục theo thực tế.
- Thêm link vào mục "Subsystem handoff" trong `handoff.md`.

## Khi xóa/merge subsystem
- Xóa file handoff tương ứng, cập nhật lại link trong `handoff.md`.

---

*Quy trình này giúp LLM và contributor mới truy vấn nhanh, tiết kiệm token, không cần đọc cả repo. Handoff nên được review khi merge PR lớn hoặc milestone.*
