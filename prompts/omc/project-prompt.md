<project-context>
<workstation>
{{#list environment prefix="- " join="\n"}}{{label}}: {{value}}{{/list}}
{{#if model}}- Model: {{model}}{{/if}}
</workstation>
</project-context>

{{#if appendPrompt}}
{{appendPrompt}}
{{/if}}
