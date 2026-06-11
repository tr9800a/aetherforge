package llm

import (
	"context"
	"reflect"
	"strings"
	"testing"

	"aetherforge/backend/internal/protocol"
)

type cannedClient struct{ recipe Recipe }

func (c *cannedClient) GenerateRecipe(context.Context, Request) (*Recipe, error) {
	r := c.recipe
	return &r, nil
}

func TestRecordThenReplay(t *testing.T) {
	dir := t.TempDir()
	want := Recipe{Items: []CategoryRecipe{
		{Category: "deciduous_tree", Count: 42, Clustering: 0.7, Tags: []string{"flora"}},
		{Category: "dog_large", Count: 2, Clustering: 0},
	}}
	req := Request{
		Prompt:   "a forest with two dogs",
		Manifest: map[string]protocol.ManifestEntry{"deciduous_tree": {}, "dog_large": {}},
	}

	rec := &RecordingClient{Inner: &cannedClient{recipe: want}, Dir: dir}
	if _, err := rec.GenerateRecipe(context.Background(), req); err != nil {
		t.Fatalf("record: %v", err)
	}

	got, err := (&ReplayClient{Dir: dir}).GenerateRecipe(context.Background(), req)
	if err != nil {
		t.Fatalf("replay: %v", err)
	}
	if !reflect.DeepEqual(*got, want) {
		t.Fatalf("replayed recipe differs:\n got %+v\nwant %+v", *got, want)
	}
}

func TestReplayDistinguishesRetries(t *testing.T) {
	first := Request{Prompt: "p"}
	retry := Request{Prompt: "p", PriorError: "bad category"}
	if requestKey(first) == requestKey(retry) {
		t.Fatal("first attempt and retry must record to distinct files")
	}
}

func TestReplayMissingRecording(t *testing.T) {
	_, err := (&ReplayClient{Dir: t.TempDir()}).GenerateRecipe(context.Background(), Request{Prompt: "never recorded"})
	if err == nil || !strings.Contains(err.Error(), "no recording") {
		t.Fatalf("want explicit no-recording error, got %v", err)
	}
}
