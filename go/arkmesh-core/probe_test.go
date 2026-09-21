package main

import (
	"strings"
	"testing"
)

// 在主机上跑一遍所有探针: 用于确认探针逻辑本身正确,
// 从而真机输出里的 FAIL 一定是 arm64/OHOS 侧的问题.
func TestProbesOnHost(t *testing.T) {
	for _, p := range runAllProbes() {
		t.Log(p)
		if strings.Contains(p, "FAIL") {
			t.Errorf("probe failed: %s", p)
		}
	}
}
